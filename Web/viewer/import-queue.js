// The order in which files are imported and drawn: the browser's copy of the Mac's library::Queue
// (Engine/library/queue.h). Both run the cases in Tests/data/gallery-queue.json, so a gallery
// fills in the same order everywhere. Pure policy: who runs a job, and how, is the caller's.
//
// What is wanted is replaced whole as the view moves: each job has a band (a file being opened, one
// in view, one near the view, or one prefetched while nothing else waits) and an order within its
// band. Jobs start best first while slots are free. A job no longer wanted is dropped before it
// starts; once running it runs on, since its result is kept, unless a job in view needs its slot.
// Only a file being opened or one in view ever cancels a running job, and only one in a lower band.

export const Bands = Object.freeze({ open: 0, visible: 1, near: 2, prefetch: 3 });

/** Files larger than this are never prefetched: they are imported only when in view. */
export const kMaxPrefetchBytes = 300_000_000;

const kStale = 4; // a running job no longer wanted ranks below every band

export class ImportQueue {
  #slots;
  #prefetch;
  #jobs = new Map(); // id -> { band, order, bytes, position, wanted, running, cancelling }
  #waiting = []; // ids, best first
  #running = 0;

  constructor({ slots = 1, prefetch = true } = {}) {
    this.#slots = slots;
    this.#prefetch = prefetch;
  }

  /**
   * Replaces what is wanted: jobs as { id, band, order, bytes }, band a name from Bands. A waiting
   * job not wanted is dropped; a running one runs on, stale. A job is started again only if it is
   * wanted after it finished.
   */
  want(jobs) {
    for (const job of this.#jobs.values()) job.wanted = false;
    jobs.forEach(({ id, band, order = 0, bytes = 0 }, position) => {
      const rank = Bands[band];
      if (rank === undefined) throw new Error(`No band named ${band}`);
      const job = this.#jobs.get(id);
      if (job) {
        Object.assign(job, { band: rank, order, bytes, position, wanted: true });
      } else {
        this.#jobs.set(id, { band: rank, order, bytes, position, wanted: true, running: false, cancelling: false });
      }
    });
    for (const [id, job] of this.#jobs) {
      if (!job.wanted && !job.running) this.#jobs.delete(id);
    }
    this.#sortWaiting();
  }

  /** Starts the best waiting job, if a slot is free and one may start, and returns its id, or null. */
  next() {
    if (this.#running >= this.#slots) return null;
    for (let i = 0; i < this.#waiting.length; i++) {
      const id = this.#waiting[i];
      const job = this.#jobs.get(id);
      if (job.band === Bands.prefetch) {
        if (!this.#prefetch) break; // prefetched jobs come last
        if (job.bytes > kMaxPrefetchBytes) continue;
      }
      this.#waiting.splice(i, 1);
      job.running = true;
      this.#running++;
      return id;
    }
    return null;
  }

  /** A running job completed, or failed: it is done with. */
  finish(id) {
    const job = this.#jobs.get(id);
    if (!job?.running) return;
    this.#running--;
    this.#jobs.delete(id);
  }

  /** A running job stopped before completing, as when cancelled: if still wanted it waits again. */
  stopped(id) {
    const job = this.#jobs.get(id);
    if (!job?.running) return;
    this.#running--;
    if (!job.wanted) {
      this.#jobs.delete(id);
      return;
    }
    job.running = false;
    job.cancelling = false;
    this.#sortWaiting();
  }

  /**
   * Running jobs to cancel now, worst first, so that waiting jobs in view can start. Each is
   * returned once, and keeps its slot until it has stopped.
   */
  toCancel() {
    let available = Math.max(0, this.#slots - this.#running);
    const candidates = [];
    for (const [id, job] of this.#jobs) {
      if (!job.running) continue;
      if (job.cancelling) {
        available++;
        continue;
      }
      candidates.push({ id, rank: job.wanted ? job.band : kStale, order: job.order, position: job.position });
    }
    candidates.sort((a, b) => compare(b, a)); // worst first
    const cancelled = [];
    for (const id of this.#waiting) {
      const band = this.#jobs.get(id).band;
      if (band > Bands.visible) break;
      if (available > 0) {
        available--;
        continue;
      }
      const worse = candidates.findIndex((c) => c.rank > band);
      if (worse < 0) break; // later jobs are in the same band or lower, and can cancel no more
      cancelled.push(candidates[worse].id);
      candidates.splice(worse, 1);
    }
    for (const id of cancelled) this.#jobs.get(id).cancelling = true;
    return cancelled;
  }

  /** Fewer slots, as memory runs short, start nothing new until running jobs fall below them. */
  setSlots(slots) {
    this.#slots = slots;
  }
  get slots() {
    return this.#slots;
  }
  /** Whether prefetched jobs may start. */
  setPrefetch(allowed) {
    this.#prefetch = allowed;
  }

  waiting() {
    return [...this.#waiting];
  }
  running() {
    return [...this.#jobs]
      .filter(([, job]) => job.running)
      .sort(([a, x], [b, y]) => compare({ id: a, rank: x.band, ...x }, { id: b, rank: y.band, ...y }))
      .map(([id]) => id);
  }
  get idle() {
    return this.#waiting.length === 0 && this.#running === 0;
  }
  /** A job's band, by name, if the queue holds it. */
  bandOf(id) {
    const job = this.#jobs.get(id);
    return job ? Object.keys(Bands).find((name) => Bands[name] === job.band) : undefined;
  }

  #sortWaiting() {
    this.#waiting = [...this.#jobs]
      .filter(([, job]) => !job.running)
      .map(([id, job]) => ({ id, rank: job.band, order: job.order, position: job.position }))
      .sort(compare)
      .map((job) => job.id);
  }
}

// Best first: band, then order, then where it was wanted, then id, as the Mac's queue keys jobs.
function compare(a, b) {
  return a.rank - b.rank || a.order - b.order || a.position - b.position || (a.id < b.id ? -1 : a.id > b.id ? 1 : 0);
}
