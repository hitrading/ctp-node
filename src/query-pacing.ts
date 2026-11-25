/*
 * query-pacing.ts - spacing between query sends, from the limits the connected
 * front grants this session.
 */

/** Spacing used until a front has granted limits: CTP's traditional one query
 *  per second, with margin. Also kept for fronts that grant only that much. */
export const DEFAULT_QUERY_INTERVAL_MS = 1100;

/** Pause before a `sync*` helper asks a counter that was not ready again. CTP has
 *  no event for becoming ready, so asking again is the only way to find out; the
 *  pause must not shrink when the front allows faster queries. */
export const QUERY_RETRY_DELAY_MS = 1100;

/** CTP's explicit "not ready, ask again later" replies to a query: 90 (query not
 *  ready, also sent above the front's query limit) and 154 (query core busy). */
const NOT_READY_ERROR_IDS = new Set([90, 154]);

/** Whether a failed query may simply be asked again: the local API's flow-control
 *  send codes, or the counter saying it is not ready. */
export function isTransientQueryFailure(error: unknown): boolean {
  const { ctpReturnCode, errorId } = (error ?? {}) as { ctpReturnCode?: unknown; errorId?: unknown };
  return ctpReturnCode === -2 || ctpReturnCode === -3 || NOT_READY_ERROR_IDS.has(errorId as number);
}

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
