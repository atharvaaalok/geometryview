// Runs the page's imports through ImportQueue, in two lanes: what a person opens, and what the
// gallery wants. What a person opens never waits behind the gallery: on a desktop the gallery has
// an importer of its own, which starts nothing new while anything is being opened; on a phone, one
// importer is shared, so the gallery's running job is cancelled for an open and waits again.
//
// The live lane's jobs are files being opened ("open"), in the order asked, and refinements of what
// is open ("visible"): a file being opened cancels a refinement running, which is not tried again;
// the engine asks anew. Who runs a job, and how, is the caller's: `run(signal)` returns a promise,
// and stops early, rejecting or not, once `signal` aborts.

import { ImportQueue } from "./import-queue.js";

export class Importers {
  #live = new ImportQueue({ slots: 1 });
  #liveWants = [];
  #gallery;
  #shared;
  #jobs = new Map(); // id -> { lane, run, resolve, reject, controller }
  #sequence = 0;

  /** `shared`: one importer for both lanes, as on a phone, where prefetching is off too. */
  constructor({ shared = false, gallerySlots = 1 } = {}) {
    this.#shared = shared;
    this.#gallery = new ImportQueue({ slots: gallerySlots, prefetch: !shared });
  }

  /** Runs a live job: a file being opened (band "open") or a refinement ("visible"). */
  live(id, run, { band = "open" } = {}) {
    return new Promise((resolve, reject) => {
      this.#jobs.set(id, { lane: "live", run, resolve, reject, controller: null });
      this.#liveWants.push({ id, band, order: this.#sequence++ });
      this.#live.want(this.#liveWants);
      this.#pump();
    });
  }

  /** Stops a live job: dropped if it waits, aborted if it runs. */
  cancel(id) {
    const job = this.#jobs.get(id);
    if (!job) return;
    if (job.controller) {
      job.controller.abort();
    } else {
      this.#dropLive(id);
      this.#jobs.delete(id);
      job.resolve({ type: "cancelled" });
      this.#pump();
    }
  }

  /**
   * Replaces what the gallery wants: [{ id, band, order, bytes, run }]. A job not wanted any more
   * is dropped before it starts and runs on once started, unless a job in view needs its slot.
   * Each job's result goes to `onDone(id, result)`; its failure to `onFailed(id, error)`.
   */
  want(jobs, { onDone = () => {}, onFailed = () => {} } = {}) {
    this.#onDone = onDone;
    this.#onFailed = onFailed;
    for (const job of jobs) {
      const known = this.#jobs.get(job.id);
      if (!known) this.#jobs.set(job.id, { lane: "gallery", run: job.run, controller: null });
      else if (known.lane === "gallery") known.run = job.run;
    }
    this.#gallery.want(jobs.map(({ id, band, order, bytes }) => ({ id, band, order, bytes })));
    for (const [id, job] of this.#jobs) {
      if (job.lane === "gallery" && !job.controller && !this.#gallery.bandOf(id)) this.#jobs.delete(id);
    }
    this.#pump();
  }
  #onDone = () => {};
  #onFailed = () => {};

  setPrefetch(allowed) {
    this.#gallery.setPrefetch(allowed && !this.#shared);
    this.#pump();
  }

  /** Whether anything is being opened, waiting or running. */
  get opening() {
    return this.#live.waiting().length > 0 || this.#live.running().length > 0;
  }

  /** Ids running now, by lane, for checks. */
  running() {
    return { live: this.#live.running(), gallery: this.#gallery.running() };
  }

  #pump() {
    for (const id of this.#live.toCancel()) this.#jobs.get(id)?.controller?.abort();
    for (let id = this.#live.next(); id !== null; id = this.#live.next()) this.#start("live", id);

    if (this.opening) {
      // One importer for both: the gallery's running job makes way, and waits again.
      if (this.#shared) for (const id of this.#gallery.running()) this.#jobs.get(id)?.controller?.abort();
      return;
    }
    for (const id of this.#gallery.toCancel()) this.#jobs.get(id)?.controller?.abort();
    for (let id = this.#gallery.next(); id !== null; id = this.#gallery.next()) this.#start("gallery", id);
  }

  #dropLive(id) {
    this.#liveWants = this.#liveWants.filter((want) => want.id !== id);
    this.#live.want(this.#liveWants);
  }

  #start(lane, id) {
    const job = this.#jobs.get(id);
    const queue = lane === "live" ? this.#live : this.#gallery;
    if (!job) {
      queue.finish(id);
      return;
    }
    const controller = new AbortController();
    job.controller = controller;
    Promise.resolve()
      .then(() => job.run(controller.signal))
      .then((result) => ({ result }), (error) => ({ error }))
      .then(({ result, error }) => {
        job.controller = null;
        const aborted = controller.signal.aborted;
        if (lane === "live") {
          queue.finish(id);
          this.#dropLive(id);
          this.#jobs.delete(id);
          if (aborted) job.resolve({ type: "cancelled" });
          else if (error) job.reject(error);
          else job.resolve(result);
        } else if (aborted) {
          queue.stopped(id); // waits again, if still wanted
          if (!queue.bandOf(id)) this.#jobs.delete(id);
        } else {
          queue.finish(id);
          this.#jobs.delete(id);
          if (error) this.#onFailed(id, error);
          else this.#onDone(id, result);
        }
        this.#pump();
      });
  }
}
