; Embed C highlighting inside __C__ { ... } and extern "C" { ... } blocks
((c_block_body) @injection.content
 (#set! injection.language "c"))

; Line/block comments
((comment) @injection.content
 (#set! injection.language "comment"))
