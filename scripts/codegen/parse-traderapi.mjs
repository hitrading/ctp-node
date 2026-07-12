/*
 * parse-traderapi.mjs - extract the trader SPI callbacks and Req* methods from
 * ThostFtdcTraderApi.h so the trader binding can be fully generated.
 */

import { readFile } from "node:fs/promises";

export async function parseTraderApi(path) {
  const text = await readFile(path, "utf8");

  // SPI callbacks: virtual void On...( ... )
  const spi = [];
  const reSpi = /virtual\s+void\s+(On\w+)\s*\(([^;{)]*)\)/g;
  let m;
  while ((m = reSpi.exec(text))) {
    const name = m[1];
    const params = m[2].replace(/\s+/g, " ").trim();
    // First CThostFtdc*Field that is not the RspInfo carrier is the payload.
    const structs = [...params.matchAll(/CThostFtdc(\w+)Field\s*\*/g)]
      .map((x) => x[1])
      .filter((x) => x !== "RspInfo");
    spi.push({
      name,
      struct: structs[0] ?? null,
      hasRsp: /CThostFtdcRspInfoField\s*\*/.test(params),
      hasReqId: /int\s+nRequestID/.test(params),
      hasIsLast: /bool\s+bIsLast/.test(params),
      intArg: /^int\s+\w+$/.test(params),
    });
  }

  // Request methods: virtual int Req...(CThostFtdc*Field *p, int nRequestID)
  const req = [];
  const reReq =
    /virtual\s+int\s+(Req\w+)\s*\(\s*CThostFtdc(\w+)Field\s*\*\s*\w+\s*,\s*int\s+nRequestID\s*\)/g;
  while ((m = reReq.exec(text))) {
    req.push({ name: m[1], struct: m[2] });
  }

  // An SDK upgrade may introduce a signature our narrow parser does not yet
  // understand. Detect that explicitly instead of publishing a binding that
  // silently omits the method/callback.
  const declaredSpi = new Set(
    [...text.matchAll(/virtual\s+void\s+(On\w+)\s*\(/g)].map((x) => x[1])
  );
  const parsedSpi = new Set(spi.map((x) => x.name));
  const missingSpi = [...declaredSpi].filter((name) => !parsedSpi.has(name));
  const declaredReq = new Set(
    [...text.matchAll(/virtual\s+int\s+(Req\w+)\s*\(/g)].map((x) => x[1])
  );
  const parsedReq = new Set(req.map((x) => x.name));
  const missingReq = [...declaredReq].filter((name) => !parsedReq.has(name));
  if (missingSpi.length || missingReq.length) {
    throw new Error(
      `unparsed Trader API declarations: ${[...missingSpi, ...missingReq].join(", ")}`
    );
  }

  return { spi, req };
}
