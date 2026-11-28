//! Scheduling decisions for the process table (#617). Pure logic: the kernel's `sched` keeps the PCBs and calls these.

/// The next process to run after `current`: round robin over the runnable ones, the idle process only when nothing
/// else can run, None when nothing (not even an idle process) can. `runnable(i)` is asked for each slot `0..n`; the
/// idle process's own entry is ignored there (it is always runnable).
pub fn pick_next(n: usize, current: usize, idle: Option<usize>, runnable: impl Fn(usize) -> bool) -> Option<usize> {
    (1..=n).map(|step| (current + step) % n).find(|&i| Some(i) != idle && runnable(i)).or(idle)
}

/// One tick charged to a process with `ticks` so far (after this tick) and `budget`: true when it is over budget and
/// must end. No budget means it may run for ever (the idle process, the harness's app B).
pub fn over_budget(ticks: u32, budget: Option<u32>) -> bool {
    budget.is_some_and(|b| ticks > b)
}

/// A whole run's deadline in mtime ticks (`deadline` = None: no deadline): true once `now` has reached it.
pub fn past_deadline(now: u64, deadline: Option<u64>) -> bool {
    deadline.is_some_and(|d| now >= d)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn round_robin_skips_what_cannot_run() {
        let ready = [true, false, true, true];
        assert_eq!(pick_next(4, 0, None, |i| ready[i]), Some(2));
        assert_eq!(pick_next(4, 2, None, |i| ready[i]), Some(3));
        assert_eq!(pick_next(4, 3, None, |i| ready[i]), Some(0));
    }

    #[test]
    fn the_only_runnable_process_runs_again() {
        assert_eq!(pick_next(3, 1, None, |i| i == 1), Some(1));
    }

    #[test]
    fn idle_runs_only_when_nothing_else_can() {
        // Slot 2 is idle: with 0 and 1 runnable it is passed over, even when it is next in the round.
        assert_eq!(pick_next(3, 1, Some(2), |_| true), Some(0));
        assert_eq!(pick_next(3, 0, Some(2), |i| i == 0), Some(0));
        assert_eq!(pick_next(3, 0, Some(2), |_| false), Some(2));
        // The idle process leaves as soon as another one can run.
        assert_eq!(pick_next(3, 2, Some(2), |i| i == 1), Some(1));
    }

    #[test]
    fn nothing_runnable_and_no_idle_is_none() {
        assert_eq!(pick_next(2, 0, None, |_| false), None);
    }

    #[test]
    fn budget_ends_a_process_on_the_tick_past_it() {
        assert!(!over_budget(50, Some(50)));
        assert!(over_budget(51, Some(50)));
        assert!(!over_budget(u32::MAX, None));
    }

    #[test]
    fn deadline_is_reached_at_its_tick() {
        assert!(!past_deadline(99, Some(100)));
        assert!(past_deadline(100, Some(100)));
        assert!(!past_deadline(u64::MAX, None));
    }
}
