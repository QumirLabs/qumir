(block
  (type key <struct (valid bool) (v i64)>)

  (fun <main> ()
    (block
      (var k key)
      (= k (: (struct ((valid #t) (v (: 7 i64)))) key))
      (if (call key_valid k)
        (output "valid\n")
        (output "invalid\n"))))

  (fun key_valid ((var k key)) -> bool
    (block (return (&& #t (field k valid))))))
