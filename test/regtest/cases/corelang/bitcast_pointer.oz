(block
  (fun <main> ()
    (block
      (var p = (cast (: 1234 i64) <ptr i8>))
      (var words = (bitcast p <ptr i64>))
      (output (bitcast p i64) " " (cast (bitcast words u64) i64) "\n")
      (output (bitcast (bitcast 0 <ptr i8>) i64) " " (!= (bitcast "abc" i64) 0) "\n"))))
