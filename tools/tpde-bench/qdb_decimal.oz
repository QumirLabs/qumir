(block
  (pragma language overloads)
  (use qumirdb)
  ;; TPC-H Q1-like sums with the decimal operators of qumirdb.oz (BinInt, i128):
  ;; price * (1 - disc) and price * (1 - disc) * (1 + tax) in four groups.
  (fun <main> ()
    (block
      (var groups = 4)
      (var base = (cast (call array_create (* groups 16)) <ptr BinInt>))
      (var disc_price = (cast (call array_create (* groups 16)) <ptr BinInt>))
      (var charge = (cast (call array_create (* groups 16)) <ptr BinInt>))
      (var one = (call qdb_decimal_from_unscaled 100))
      (var n = 2000000)
      (var i = 0)
      (while (< i n)
        (block
          (var price = (call qdb_decimal_from_unscaled (+ 90000 (% (* i 7919) 1000000))))
          (var disc = (call qdb_decimal_from_unscaled (% i 11)))
          (var tax = (call qdb_decimal_from_unscaled (% i 9)))
          (var g = (& (+ i (>> i 3)) 3))
          (var dp = (* price (- one disc)))
          (= base [g] (+ (index base g) price))
          (= disc_price [g] (+ (index disc_price g) dp))
          (= charge [g] (+ (index charge g) (* dp (+ one tax))))
          (= i (+ i 1))))
      (var g = 0)
      (while (< g groups)
        (block
          (var c = (field (index charge g) V))
          (output g " " (cast (field (index base g) V) i64) " " (cast (field (index disc_price g) V) i64)
            " " (cast (>> c (: 64 i128)) i64) " " (cast c i64) "\n")
          (= g (+ g 1)))))))
