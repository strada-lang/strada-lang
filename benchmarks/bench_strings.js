#!/usr/bin/env node

// Concatenate "hello" 500,000 times
let s = "";
for (let i = 0; i < 500_000; i++) {
    s += "hello";
}
console.log(`concat len: ${s.length}`);

// Split a large string 100,000 times
const csv = "alpha,bravo,charlie,delta,echo,foxtrot,golf,hotel";
let total_parts = 0;
for (let j = 0; j < 100_000; j++) {
    const parts = csv.split(",");
    total_parts += parts.length;
}
console.log(`split parts: ${total_parts}`);

// Regex replace on a template string 200,000 times
const template = "Hello NAME, welcome to PLACE on DATE";
let result = "";
const pat_name = /NAME/;
const pat_place = /PLACE/;
const pat_date = /DATE/;
for (let m = 0; m < 200_000; m++) {
    result = template;
    result = result.replace(pat_name, "World");
    result = result.replace(pat_place, "Strada");
    result = result.replace(pat_date, "today");
}
console.log(`regex result: ${result}`);
