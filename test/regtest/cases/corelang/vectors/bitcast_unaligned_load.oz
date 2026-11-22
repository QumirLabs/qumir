(block
  (fun <main> ()
    (block
      ;; A table in a string literal is not aligned, so a lookup loads a vector of
      ;; bytes at any offset and reinterprets it as one scalar.
      (var text = (bitcast "ABCDEFGHIJKLMNOPQRSTUVWX" <ptr u8>))
      (var i = 0)
      (while (< i 3)
        (block
          (var lanes = (cast (+ (cast text i64) i) <ptr <vec u8 8>>))
          (output (bitcast (index lanes 0) i64) " " (bitcast (index lanes 0) f64) "\n")
          (= i (+ i 1)))))))
