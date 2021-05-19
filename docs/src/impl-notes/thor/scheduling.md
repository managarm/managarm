# Scheduling


## Theory

We first develop the theoretical model that Managarm's scheduling heuristic
is based on. Even though our goal is to work with discrete time slices, it will be
beneficial to imagine that time slices can arbitrarily be split up into smaller
sub-slices.

Let:

* \\(s_i\\) be the length of the \\(i\\)-th time slice,
* \\(n_i\\) be the number of threads that are schedulable in time slice \\(i\\)
  (i.e., threads that are either running or runnable, but not blocking),
* \\(\alpha_i(t)\\) be 1 if thread \\(t\\) is schedulable in time slice \\(i\\),
  and 0 otherwise,
* and let \\(r_\ell(t)\\) be the actual running time of thread \\(t\\) up to
  (and including) time slice \\(\ell\\).

**Deciding which thread to run.**
From a fairness perspective, it would be ideal to run all \\(n_i\\) threads
for time \\(\frac{s_i}{n_i}\\) in time slice \\(i\\)
(i.e., it would be ideal to split up the time slice into \\(n_i\\) sub-slices).
In reality, we can only run a single thread per time slice; however, we can still
try to balance the running time of each thread such that it
approaches the ideal scenario. For this purpse,
Managarm defines the *unfairness* of thread \\(t\\) as:

\\[ u_\ell(t) = \left( \sum_{i=0}^\ell \alpha_i(t) \frac{s_i}{n_i} \right) - r_\ell(t) \\]

In our idealized scenario, we have \\(u(t) = 0\\) at all times:
if we split the \\(i\\)-th time slice into \\(n_i\\) sub-slices
of length \\(\frac{s_i}{n_i}\\) and let each thread run
in exactly one of the sub-slices, then \\(r_\ell(t)\\) would
exactly equal the sum of all \\(\frac{s_i}{n_i}\\).
Thus, if \\(u(t) < 0\\) (respectively \\(u(t) > 0\\)), thread \\(t\\) has been active for
longer (respectively shorter) than it would have been in the ideal scenario.
Since \\(u(t)\\) decreases if thread \\(t\\) runs (and increases if
a thread other than \\(t\\) runs), we always schedule the thread with
the highest value of \\(u(t)\\).

**Picking the length of a time slice.**
Our scheduling heuristic also has to pick the length of time slice \\(s_\ell\\).
For this purpose, consider the thread \\(t_0\\) that runs in that time slice.
We want to run \\(t_0\\) until \\(u(t_1) \geq u(t_0)\\), where
\\(t_1\\) is the next best thread. While thread \\(t_0\\) runs,
\\(u(t_0)\\) evolves as:

\\[ u_\ell(t_0) = u_{\ell-1}(t_0) + \frac{s_\ell}{n_\ell} - s_\ell \\]

on the other hand, \\(u(t_1)\\) is given by:

\\[ u_\ell(t_1) = u_{\ell-1}(t_1) + \frac{s_\ell}{n_\ell} \\]

Hence,
\\( u(t_1) - u(t_0) \geq 0 \Leftrightarrow s_\ell \geq u_{\ell - 1}(t_0) - u_{\ell - 1}(t_1) \\)
yields the length of our time slice.
In practice, we want to avoid time slices that are too short and pick
\\( s_\ell \geq u_{\ell - 1}(t_0) - u_{\ell - 1}(t_1) + s_\textrm{gr} \\),
where \\( s_\textrm{gr} \\) is some granularity constant (e.g., 10 ms).
