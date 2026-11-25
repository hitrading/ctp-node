/*
 * query-pacing.ts - spacing between query sends, from the limits the connected
 * front grants this session.
 */

/** Spacing used until a front has granted limits: CTP's traditional one query
 *  per second, with margin. Also kept for fronts that grant only that much. */
export const DEFAULT_QUERY_INTERVAL_MS = 1100;

/** Pause before a `sync*` helper asks again: a retry, or the confirmation of an
 *  empty snapshot. It gives the counter time to become ready after login, so it
 *  must not shrink when the front allows faster queries. */
export const QUERY_RETRY_DELAY_MS = 1100;

/** Never pace faster than this, however generous (or unlimited) the front is. */
const MAX_QUERIES_PER_SECOND = 10;

/**
 * The front reports two per-second limits for a session: queries (`qryFreq`) and
 * all FTD packets (`ftdPkgFreq`: login, query, order and cancel alike). A query
 * spends one of each, and packets above `ftdPkgFreq` are silently held by the
 * front until the next second - including orders. Queries therefore take at most
 * half of the packet budget, leaving the rest to orders and cancels.
 *
 * The 10% spacing margin keeps every one-second window strictly inside the limit.
 */
export function queryIntervalMs(limits: { qryFreq: number; ftdPkgFreq: number }): number {
  const perSecond = Math.min(limits.qryFreq, Math.floor(limits.ftdPkgFreq / 2), MAX_QUERIES_PER_SECOND);
  return perSecond >= 1 ? Math.ceil(DEFAULT_QUERY_INTERVAL_MS / perSecond) : DEFAULT_QUERY_INTERVAL_MS;
}
