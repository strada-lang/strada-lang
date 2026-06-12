#!/usr/bin/env node
// Node counterpart of bench_data.strada (identical workload).
const fs = require("fs");
const readline = require("readline");

async function main() {
    const path = "/tmp/node_bench_data.csv";
    const rows = 500000;
    const t0 = Date.now();

    let seed = 42;
    const parts = [];
    for (let i = 0; i < rows; i++) {
        seed = (seed * 1103515245 + 12345) % 2147483648;
        parts.push(`2026-06-12T01:00:00,user${seed % 5000},action${Math.floor(seed/7) % 6},${seed % 50000},ok\n`);
    }
    fs.writeFileSync(path, parts.join(""));
    const t1 = Date.now();
    console.log("generate:", rows, (t1 - t0) / 1000);

    const byUser = {}, byAction = {}, bytesByUser = {};
    let parsed = 0;
    const rl = readline.createInterface({ input: fs.createReadStream(path), crlfDelay: Infinity });
    for await (const line of rl) {
        const f = line.split(",");
        if (f.length < 5) continue;
        byUser[f[1]] = (byUser[f[1]] || 0) + 1;
        byAction[f[2]] = (byAction[f[2]] || 0) + 1;
        bytesByUser[f[1]] = (bytesByUser[f[1]] || 0) + (+f[3]);
        parsed++;
    }
    const t2 = Date.now();
    console.log("aggregate:", parsed, "users=" + Object.keys(byUser).length, (t2 - t1) / 1000);

    const ranked = Object.keys(bytesByUser).sort((a, b) => bytesByUser[b] - bytesByUser[a]);
    let report = "";
    for (let i = 0; i < 20; i++) {
        const u = ranked[i];
        report += `${u} events=${byUser[u]} bytes=${bytesByUser[u]}\n`;
    }
    const t3 = Date.now();
    console.log("report:", report.length, (t3 - t2) / 1000);
    fs.unlinkSync(path);
    console.log("total:", (t3 - t0) / 1000);
}
main();
