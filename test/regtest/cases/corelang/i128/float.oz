(block
  ; i128 <-> f64. To f64: round to nearest, ties to even. To i128: truncation;
  ; NaN gives 0, values out of range give MIN or MAX.
  (fun <main> ()
    (block
      (var min = (<< (: 1 i128) (: 127 i128)))
      (var max = (~ min))
      (var p100 = (<< (: 1 i128) (: 100 i128)))
      ; doubles at 2^100 are 2^48 apart
      (var half = (<< (: 1 i128) (: 47 i128)))

      (output "1e21:        " (== (call tof (* (: 1000000000000 i128) (: 1000000000 i128))) 1e21) "\n")
      (output "tie even:    " (== (call tof (+ p100 half)) (call tof p100)) "\n")
      (output "tie odd:     " (== (call tof (+ p100 (* (: 3 i128) half))) (call tof (+ p100 (<< half (: 2 i128))))) "\n")
      (output "above tie:   ") (call back (call tof (+ (+ p100 half) (: 1 i128))))
      (output "-above tie:  ") (call back (call tof (- (- p100) (+ half (: 1 i128)))))
      (output "min:         ") (call back (call tof min))
      ; MAX rounds up to 2^127, which saturates back to MAX
      (output "max:         ") (call back (call tof max))

      (var nan = (bitcast -1 f64))
      (var inf = (bitcast 9218868437227405312 f64))
      (output "123.9:       ") (call back 123.9)
      (output "-123.9:      ") (call back -123.9)
      (output "-1.5e30:     ") (call back -1.5e30)
      (output "nan:         ") (call back nan)
      (output "inf:         ") (call back inf)
      (output "-inf:        ") (call back (- inf))
      (output "1e39:        ") (call back 1e39)
      (output "-1e39:       ") (call back -1e39)
      ; 2^127 is the first double over MAX; -2^127 is MIN itself
      (output "2^127:       ") (call back 1.7014118346046923e38)
      (output "-2^127:      ") (call back -1.7014118346046923e38)
      (output "below 2^127: ") (call back 1.7014118346046921e38)

      (var umax = (~ (: 0 u128)))
      (var top = (cast min u128))
      (output "umax:        " (== (call utof umax) 3.402823669209385e38) "\n")
      ; doubles at 2^127 are 2^75 apart
      (output "u tie even:  " (== (call utof (+ top (cast (<< (: 1 i128) (: 74 i128)) u128))) (call utof top)) "\n")
      (output "u 1e38:      ") (call uback 1e38)
      (output "u 0.9:       ") (call uback 0.9)))

  (fun tof ((var x i128)) -> f64
    (block (return (cast x f64))))

  (fun utof ((var x u128)) -> f64
    (block (return (cast x f64))))

  (fun back ((var x f64))
    (block
      (call put (cast x i128))
      (output "\n")))

  (fun uback ((var x f64))
    (block
      (call put (cast (cast x u128) i128))
      (output "\n")))

  ; Decimal digits; the remainders of a negative value are negative, so MIN
  ; needs no negation.
  (fun put ((var x i128))
    (block
      (if (< x (: 0 i128))
        (block (output "-")))
      (call digits x)))

  (fun digits ((var x i128))
    (block
      (var rest = (// x (: 10 i128)))
      (if (!= rest (: 0 i128))
        (block (call digits rest)))
      (var d = (cast (% x (: 10 i128)) i64))
      (if (< d 0)
        (block (= d (- d))))
      (output d))))
