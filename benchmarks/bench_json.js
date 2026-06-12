#!/usr/bin/env node
// Node counterpart of bench_json.strada (built-in JSON — note this is
// native C++, vs Strada's pure-Strada module; see BASELINE.md note).
function buildDoc(users) {
    const list = [];
    for (let i = 0; i < users; i++) {
        list.push({
            id: i, name: "user_" + i, email: `user${i}@example.com`,
            active: (i % 3) === 0 ? 1 : 0, score: i * 1.5,
            tags: ["alpha", "beta", "tag" + (i % 50)],
            profile: { city: "city" + (i % 100), zip: 10000 + (i % 90000), langs: ["en", "de"] },
        });
    }
    return { count: users, users: list };
}

const doc = buildDoc(2000);
let t0 = Date.now();
let encLen = 0, json = "";
for (let r = 0; r < 20; r++) { json = JSON.stringify(doc); encLen += json.length; }
let t1 = Date.now();
console.log("encode:", encLen, (t1 - t0) / 1000);

let decUsers = 0;
for (let r = 0; r < 20; r++) { decUsers += JSON.parse(json).count; }
let t2 = Date.now();
console.log("decode:", decUsers, (t2 - t1) / 1000);

let rt = 0;
for (let i = 0; i < 20000; i++) {
    const small = JSON.stringify({ op: "get", id: i, args: [1, 2, 3] });
    rt += JSON.parse(small).id % 2;
}
let t3 = Date.now();
console.log("roundtrip:", rt, (t3 - t2) / 1000);
console.log("total:", (t3 - t0) / 1000);
