context("Test versus old results")

formals(expect_known_value)$update <- FALSE

test_that("'FSKA' gives the same", {
  set.seed(90638579)
  n <- 5000L
  p <- 4L
  X <- matrix(rnorm(n * p), nrow = p)
  ws <- exp(rnorm(n))
  ws <- ws / sum(ws)

  exact <- FSKA:::naive(X = X, ws = ws, Y = X, n_threads = 2L)
  expect_known_value(exact, "FSKA-old-res-small-eps-exact.RDS")

  o1 <- FSKA:::FSKA(X = X, ws = ws, Y = X, N_min = 10L, eps = 1e-3,
                    n_threads = 2L)
  # all.equal(o1, exact)
  # o1 <- o1 / sum(o1)
  # exact <- exact / sum(exact)
  # all.equal(o1, exact)
  expect_known_value(o1, "FSKA-old-res-small-eps.RDS")

  o2 <- FSKA:::FSKA(X = X, ws = ws, Y = X, N_min = 10L, eps = 1e-3,
                     n_threads = 1L)
  expect_known_value(o2, "FSKA-old-res-small-eps.RDS")
  expect_equal(o1, o2)

  o3 <- FSKA:::FSKA(X = X, ws = ws, Y = X, N_min = 10L, eps = 1e-12,
                    n_threads = 2L)
  expect_equal(o3, exact)
})

test_that("we get the same KD-tree", {
  set.seed(90638579)
  n <- 5000L
  p <- 4L
  X <- matrix(rnorm(n * p), nrow = p)

  t1 <- FSKA:::test_KD_note(X, 10L)
  t2 <- FSKA:::test_KD_note(X, 10L)
  expect_equal(t1, t2)

  expect_known_value(t1, "KD-tree.RDS")
})
