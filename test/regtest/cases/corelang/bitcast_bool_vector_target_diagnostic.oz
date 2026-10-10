; disable_exec
(block
  (use Vector)
  (fun <main> ()
    (block
      (var bytes = (: (vec 2 0 1 255) <vec u8 4>))
      (var mask = (bitcast bytes <vec bool 4>)))))
