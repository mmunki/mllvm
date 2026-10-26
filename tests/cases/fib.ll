define i64 @fib(i64 %n) {
entry:
  %c = icmp slt i64 %n, 2
  br i1 %c, label %base, label %rec
base:
  ret i64 %n
rec:
  %a = sub i64 %n, 1
  %b = sub i64 %n, 2
  %fa = call i64 @fib(i64 %a)
  %fb = call i64 @fib(i64 %b)
  %r = add i64 %fa, %fb
  ret i64 %r
}

define i32 @main() {
entry:
  %r = call i64 @fib(i64 10)
  %t = trunc i64 %r to i32
  ret i32 %t
}
