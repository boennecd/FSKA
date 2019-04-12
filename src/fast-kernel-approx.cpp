#include "fast-kernel-approx.h"
#include <limits>
#include <math.h>
#include <cmath>
#include "kernels.h"
#include "thread_pool.h"
#include <utility>
#include <math.h>

#ifdef FSKA_PROF
#include <gperftools/profiler.h>
#include <iostream>
#include <iomanip>
#include <ctime>
#include <sstream>
#endif

struct get_X_root {
  using output_type =
    std::tuple<std::unique_ptr<KD_note>, std::unique_ptr<source_node>,
               arma::uvec>;
  arma::mat &X;
  arma::vec &ws;
  const arma::uword N_min;

  output_type operator()()
  {
    output_type out;
    auto &node = std::get<0>(out);
    auto &snode = std::get<1>(out);
    auto &old_idx = std::get<2>(out);

    node.reset(new KD_note(get_KD_tree(X, N_min)));

    /* make permutation to get original order */
    arma::uvec new_idx = node->get_indices_parent();
    old_idx.resize(X.n_cols);
    std::iota(old_idx.begin(), old_idx.end(), 0L);
    node->set_indices(old_idx);
    arma::uword i = 0L;
    for(auto n : new_idx)
      old_idx[n] = i++;

    /* permutate */
    X = X.cols(new_idx);
    ws = ws(new_idx);

    snode.reset(new source_node(X, ws, *node));

    return out;
  }

};

struct get_Y_root {
  using output_type =
    std::tuple<std::unique_ptr<KD_note>, std::unique_ptr<query_node>,
               arma::uvec>;

  arma::mat &Y;
  const arma::uword N_min;

  output_type operator()()
  {
    output_type out;
    auto &node  = std::get<0L>(out);
    auto &snode = std::get<1L>(out);
    auto &old_idx = std::get<2>(out);

    node.reset(new KD_note(get_KD_tree(Y, N_min)));

    /* make permutation to get original order */
    arma::uvec new_idx = node->get_indices_parent();
    old_idx.resize(Y.n_cols);
    std::iota(old_idx.begin(), old_idx.end(), 0L);
    node->set_indices(old_idx);
    arma::uword i = 0L;
    for(auto n : new_idx)
      old_idx[n] = i++;

    /* permutate */
    Y = Y.cols(new_idx);

    snode.reset(new query_node(Y, *node));

    return out;
  }
};

constexpr unsigned int max_futures       = 30000L;
constexpr unsigned int max_futures_clear = max_futures / 3L;

struct comp_w_centroid {
  arma::vec &log_weights;
  const source_node &X_node;
  const query_node &Y_node;
  const arma::mat &Y;
  const mvariate &kernel;
  const bool is_single_threaded;

  void operator()()
  {
    if(!Y_node.node.is_leaf()){
      (comp_w_centroid { log_weights, X_node, *Y_node.left , Y, kernel,
                         is_single_threaded })();
      (comp_w_centroid {log_weights, X_node, *Y_node.right, Y, kernel,
                        is_single_threaded})();
      return;
    }

    const std::vector<arma::uword> &idx = Y_node.node.get_indices();
    const arma::vec &X_centroid = X_node.centroid;
    double x_weight_log = std::log(X_node.weight);
    const double *xp = X_centroid.begin();
    const arma::uword N = X_centroid.n_elem;

    arma::vec out;
    double *o = nullptr;
    if(!is_single_threaded){
      out.set_size(idx.size());
      o = out.begin();
    }
    for(auto i : idx){
      double dist = norm_square(xp, Y.colptr(i), N);
      double new_term = kernel(dist, true) + x_weight_log;
      if(!is_single_threaded){
        *(o++) = new_term;
        continue;
      }

      log_weights[i] = log_sum_log(log_weights[i], new_term);

    }

    if(is_single_threaded)
      return;

    o = out.begin();
    std::lock_guard<std::mutex> guard(*Y_node.idx_mutex);
    for(auto i : idx)
      log_weights[i] = log_sum_log(log_weights[i], *(o++));
  }
};

struct comp_all {
  arma::vec &log_weights;
  const source_node &X_node;
  const query_node &Y_node;
  const arma::mat &X;
  const arma::vec &ws_log;
  const arma::mat &Y;
  const mvariate &kernel;
  const bool is_single_threaded;

  void operator()()
  {
#ifdef FSKA_DEBUG
    if(!X_node.node.is_leaf() or !Y_node.node.is_leaf())
      throw "comp_all called with non-leafs";
#endif

    const std::vector<arma::uword> &idx_y = Y_node.node.get_indices(),
      &idx_x = X_node.node.get_indices();
    arma::vec x_y_ws(idx_x.size());

    arma::vec out;
    double *o = nullptr;
    if(!is_single_threaded){
      out.set_size(idx_x.size());
      o = out.begin();
    }
    for(auto i_y : idx_y){
      const arma::uword N = Y.n_rows;
      double max_log_w = std::numeric_limits<double>::lowest();
      double *x_y_ws_i = x_y_ws.begin();
      for(auto i_x : idx_x){
        double dist = norm_square(X.colptr(i_x), Y.colptr(i_y), N);
        *x_y_ws_i = ws_log[i_x] + kernel(dist, true);
        if(*x_y_ws_i > max_log_w)
          max_log_w = *x_y_ws_i;

        x_y_ws_i++;
      }
      double new_term = log_sum_log(x_y_ws, max_log_w);
      if(!is_single_threaded){
        *(o++) = new_term;
        continue;

      }

      log_weights[i_y] = log_sum_log(log_weights[i_y], new_term);

    }

    if(is_single_threaded)
      return;

    o = out.begin();
    std::lock_guard<std::mutex> guard(*Y_node.idx_mutex);
    for(auto i_y : idx_y)
      log_weights[i_y] = log_sum_log(log_weights[i_y], *(o++));
  }
};

struct comp_weights {
  arma::vec &log_weights;
  const arma::mat &X;
  const arma::vec &ws_log;
  const arma::mat &Y;
  const double eps;
  const mvariate &kernel;
  thread_pool &pool;
  std::list<std::future<void> > &futures;
  const bool finish;

private:
  void do_work(const source_node &X_node_t, const query_node &Y_node_t)
  {
    if(!finish and futures.size() > max_futures){
      std::size_t n_earsed = 0L;
      std::future_status status;
      static constexpr std::chrono::microseconds t_weight(1);
      std::list<std::future<void> >::iterator it;
      const std::list<std::future<void> >::const_iterator
        end = futures.end();
      while(n_earsed < max_futures_clear){
        for(it = futures.begin(); it != end; ){
          status = it->wait_for(t_weight);
          if(status == std::future_status::ready){
            it->get();
            it = futures.erase(it);
            n_earsed++;
            if(n_earsed >= max_futures_clear)
              break;

          } else
            ++it;
        }
      }
    }

    auto dists = Y_node_t.borders.min_max_dist(X_node_t.borders);
    double k_min = kernel(dists[1], false), k_max = kernel(dists[0], false);

    if(X_node_t.weight *
       (k_max - k_min) / ((k_max + k_min) / 2. + 1e-16) < 2. * eps){
      comp_w_centroid task { log_weights, X_node_t, Y_node_t, Y, kernel,
                             pool.thread_count < 2L };

      if(!finish)
        futures.push_back(pool.submit(std::move(task)));
      else
        task();

      return;
    }

    if(X_node_t.node.is_leaf() and Y_node_t.node.is_leaf()){
      comp_all task { log_weights, X_node_t, Y_node_t, X, ws_log, Y, kernel,
                      pool.thread_count < 2L };

      if(!finish)
        futures.push_back(pool.submit(std::move(task)));
      else
        task();
      return;
    }

    /* check if we should finish the rest in another thread */
    static constexpr arma::uword stop_n_elem = 50L;
    if(!finish and
         X_node_t.node.n_elem < stop_n_elem and
         Y_node_t.node.n_elem < stop_n_elem){
      futures.push_back(pool.submit(
        comp_weights { log_weights, X, ws_log, Y, eps,
                       kernel, pool, futures, true, X_node_t, Y_node_t }));
      return;
    }

    if(!X_node_t.node.is_leaf() and  Y_node_t.node.is_leaf()){
      do_work(*X_node_t.left , Y_node_t);
      do_work(*X_node_t.right,  Y_node_t);
      return;
    }
    if( X_node_t.node.is_leaf() and !Y_node_t.node.is_leaf()){
      do_work(X_node_t, *Y_node_t.left );
      do_work(X_node_t, *Y_node_t.right);
      return;
    }

    do_work(*X_node_t.left , *Y_node_t.left );
    do_work(*X_node_t.left , *Y_node_t.right);
    do_work(*X_node_t.right, *Y_node_t.left );
    do_work(*X_node_t.right, *Y_node_t.right);
  }

public:
  const source_node &X_node;
  const query_node &Y_node;

  void operator()()
  {
    do_work(X_node, Y_node);
  }
};

// [[Rcpp::export]]
arma::vec FSKA(
    arma::mat X, arma::vec ws, arma::mat Y,
    const arma::uword N_min, const double eps,
    const unsigned int n_threads){
#ifdef FSKA_PROF
  std::stringstream ss;
  auto t = std::time(nullptr);
  auto tm = *std::localtime(&t);
  ss << std::put_time(&tm, "profile-FSKA-%d-%m-%Y-%H-%M-%S.log");
  Rcpp::Rcout << "Saving profile output to '" << ss.str() << "'" << std::endl;
  const std::string s = ss.str();
  ProfilerStart(s.c_str());
#endif

  thread_pool pool(n_threads);

  auto f1 = pool.submit(get_X_root { X, ws, N_min } );
  auto f2 = pool.submit(get_Y_root { Y, N_min } );
  auto X_root = f1.get();
  auto Y_root = f2.get();

  source_node &X_root_source = *std::get<1L>(X_root);
  query_node &Y_root_query   = *std::get<1L>(Y_root);
  const arma::uvec &permu_vec = std::get<2L>(Y_root);

  const arma::vec ws_log = arma::log(ws);
  const mvariate kernel(X.n_rows);

  arma::vec log_weights(Y.n_cols);
  log_weights.fill(-std::numeric_limits<double>::infinity());
  std::list<std::future<void> > futures;
  (comp_weights { log_weights, X, ws_log, Y, eps,
                  kernel, pool, futures, false, X_root_source,
                  Y_root_query } )();

  while(!futures.empty()){
    futures.back().get();
    futures.pop_back();
  }

#ifdef FSKA_PROF
  ProfilerStop();
#endif

  return log_weights(permu_vec);
}



inline std::unique_ptr<const source_node> set_child
  (const arma::mat &X, const arma::vec &ws, const KD_note &node,
   const bool is_left)
  {
    if(node.is_leaf())
      return std::unique_ptr<source_node>();
    if(is_left)
      return std::unique_ptr<source_node>(
        new source_node(X, ws, node.get_left ()));
    return std::unique_ptr<source_node>(
        new source_node(X, ws, node.get_right()));
  }

arma::vec set_centroid
  (const source_node &snode, const arma::mat &X, const arma::vec &ws)
  {
    if(snode.node.is_leaf()){
      arma::vec centroid(X.n_rows, arma::fill::zeros);
      const auto &indices = snode.node.get_indices();
      double sum_w = 0.;
      for(auto idx : indices){
        centroid += ws[idx] * X.unsafe_col(idx);
        sum_w += ws[idx];
      }
      centroid /= sum_w;

      return centroid;
    }

    double w1 = snode.left->weight, w2 = snode.right->weight;
    return
      (w1 / (w1 + w2)) * snode.left->centroid +
      (w2 / (w1 + w2)) * snode.right->centroid;
  }

inline double set_weight(const source_node &snode, const arma::vec &ws)
  {
    if(snode.node.is_leaf()){
      const auto &indices = snode.node.get_indices();
      double weight = 0.;
      for(auto idx : indices)
        weight += ws[idx];

      return weight;
    }

    return snode.left->weight + snode.right->weight;
  }

template<class T>
hyper_rectangle set_borders(const T &snode, const arma::mat &X){
  if(snode.node.is_leaf())
    return hyper_rectangle(X, snode.node.get_indices());

  return hyper_rectangle(snode.left->borders, snode.right->borders);
}

source_node::source_node
  (const arma::mat &X, const arma::vec &ws, const KD_note &node):
  node(node), left(set_child(X, ws, node, true)),
  right(set_child(X, ws, node, false)), centroid(set_centroid(*this, X, ws)),
  weight(set_weight(*this, ws)), borders(set_borders(*this, X))
  { }



inline std::unique_ptr<const query_node> set_child_query
  (const arma::mat &X, const KD_note &node, const bool is_left)
{
  if(node.is_leaf())
    return std::unique_ptr<query_node>();
  if(is_left)
    return std::unique_ptr<query_node>(
      new query_node(X, node.get_left ()));
  return std::unique_ptr<query_node>(
    new query_node(X, node.get_right()));
}

query_node::query_node(const arma::mat &Y, const KD_note &node):
  node(node), left(set_child_query(Y, node, true)),
  right(set_child_query(Y, node, false)), borders(set_borders(*this, Y)),
  idx_mutex(new std::mutex()) { }
