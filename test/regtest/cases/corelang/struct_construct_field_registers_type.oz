(block
  ; Lowering a field value that builds an unregistered struct adds a type to the
  ; table while the outer struct is still being constructed.
  (type outer <struct (x i64) (y i64)>)
  (type inner <struct (a i64) (b i64) (c i64)>)

  (fun <main> ()
    (block
      (var s = (: (struct ((x (field (: (struct ((a 1) (b 2) (c 4))) inner) b)) (y 3))) outer))
      (output (field s x) " " (field s y) "\n"))))
