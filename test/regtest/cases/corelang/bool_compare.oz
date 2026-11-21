(block
  (fun <main> ()
    (block
      (var x i64)
      (= x 3)
      (var t = (> x 2))
      (var f = (> x 5))
      (output (< f t) " " (<= f t) " " (> f t) " " (>= f t) "\n")
      (output (< t f) " " (<= t f) " " (> t f) " " (>= t f) "\n")
      (output (< t t) " " (<= f f) " " (< #f #t) " " (> #t #f) "\n")
      (if (< f t)
        (output "less\n")
        (output "not less\n")))))
