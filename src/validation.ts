/** Runtime request/config objects must have predictable own-property semantics.
 * Accept ordinary object literals and null-prototype dictionaries; reject class
 * instances and inherited fields so a value cannot reach the CTP wire without
 * appearing in the caller's explicit request shape. */
export function assertPlainRecord(
  value: unknown,
  label: string
): asserts value is Record<string, unknown> {
  if (value === null || typeof value !== "object" || Array.isArray(value))
    throw new TypeError(`${label} must be a plain object`);
  const prototype = Object.getPrototypeOf(value);
  if (prototype !== Object.prototype && prototype !== null)
    throw new TypeError(`${label} must be a plain object`);
}

/** Return every own key, including non-enumerable keys. Symbols cannot name a
 * CTP field and are rejected rather than silently ignored. */
export function ownStringKeys(value: unknown, label: string): string[] {
  assertPlainRecord(value, label);
  const keys = Reflect.ownKeys(value);
  if (keys.some((key) => typeof key !== "string"))
    throw new TypeError(`${label} contains an unsupported symbol field`);
  return keys as string[];
}

/** Snapshot only explicit values into a null-prototype object before handing a
 * request to N-API. This prevents Object.prototype pollution from being seen by
 * native Has()/Get() and treats an explicit `undefined` like an omitted optional
 * field, matching the binary codec. */
export function copyOwnRecord<T extends object>(value: T, label: string): T {
  const source = value as Record<string, unknown>;
  const copy = Object.create(null) as Record<string, unknown>;
  for (const key of ownStringKeys(value, label)) {
    const field = source[key];
    if (field !== undefined) copy[key] = field;
  }
  return copy as T;
}

/** Validate and snapshot a dense instrument-id list in one pass. Array iteration
 * helpers such as some() skip holes; treating a hole as an omitted CTP
 * InstrumentID can accidentally turn a scoped metadata request into a query-all
 * request, so holes and inherited indices are rejected (the same explicit-own-
 * value rule as request objects). Callers that cross an async boundary must use
 * this copy: otherwise an accessor or ordinary mutation after validation can
 * change a scoped request into another query. */
export function copyDenseStringArray(
  value: unknown,
  label: string,
  allowEmpty = true
): string[] {
  if (!Array.isArray(value))
    throw new TypeError(`${label} must be ${allowEmpty ? "an" : "a non-empty"} instrument-id array`);
  const length = value.length;
  if (!allowEmpty && length === 0)
    throw new TypeError(`${label} must be a non-empty instrument-id array`);
  const copy = new Array<string>(length);
  for (let i = 0; i < length; i++) {
    if (!Object.prototype.hasOwnProperty.call(value, i)) {
      throw new TypeError(`${label} must contain dense, non-empty strings without NUL`);
    }
    const item: unknown = value[i]; // read an accessor exactly once
    if (typeof item !== "string" || item.length === 0 || item.includes("\0"))
      throw new TypeError(`${label} must contain dense, non-empty strings without NUL`);
    copy[i] = item;
  }
  return copy;
}

/** Copy a constructor front list without allowing Array.prototype values to
 * become connection addresses. Empty entries are preserved for the native
 * normalization that skips them; at least one non-empty front is still
 * enforced natively. */
export function copyOwnFronts(
  value: string | string[],
  label: string
): string | string[] {
  if (typeof value === "string") {
    if (value.includes("\0")) throw new TypeError(`${label} must not contain NUL`);
    return value;
  }
  if (!Array.isArray(value))
    throw new TypeError(`${label} must be a string or string array`);
  const length = value.length;
  const copy: string[] = new Array(length);
  for (let i = 0; i < length; i++) {
    if (!Object.prototype.hasOwnProperty.call(value, i)) {
      throw new TypeError(`${label} must contain only explicit strings without NUL`);
    }
    const item: unknown = value[i]; // snapshot an accessor exactly once
    if (typeof item !== "string" || item.includes("\0"))
      throw new TypeError(`${label} must contain only explicit strings without NUL`);
    copy[i] = item;
  }
  return copy;
}
