(block
  (type big <struct (v0 i64) (v1 i64) (v2 i64) (v3 i64) (v4 i64) (v5 i64) (v6 i64) (v7 i64) (v8 i64) (v9 i64) (v10 i64) (v11 i64) (v12 i64) (v13 i64) (v14 i64) (v15 i64) (v16 i64) (v17 i64) (v18 i64) (v19 i64) (v20 i64) (v21 i64) (v22 i64) (v23 i64) (v24 i64) (v25 i64) (v26 i64) (v27 i64) (v28 i64) (v29 i64) (v30 i64) (v31 i64) (v32 i64)>)
  (type small <struct (a i64) (b i64)>)
  (fun <main> ()
    (block
      (var s small)
      (= s (: (struct ((a 1) (b 2))) small))
      (var b big)
      (= b (: (struct ((v0 0) (v1 1) (v2 2) (v3 3) (v4 4) (v5 5) (v6 6) (v7 7) (v8 8) (v9 9) (v10 10) (v11 11) (v12 12) (v13 13) (v14 14) (v15 15) (v16 16) (v17 17) (v18 18) (v19 19) (v20 20) (v21 21) (v22 22) (v23 23) (v24 24) (v25 25) (v26 26) (v27 27) (v28 28) (v29 29) (v30 30) (v31 31) (v32 32))) big))
      (output (+ (field s b) (field b v32)) "\n"))))
