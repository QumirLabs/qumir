; disable_exec
(block
  (fun <main> ()
    (block
      (var p = (cast (: 1234 i64) <ptr i8>))
      (var f = (bitcast p f64)))))
