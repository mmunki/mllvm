define i32 @main() {
entry:
  %a = add i32 20, 22
  %b = mul i32 %a, 1
  %c = icmp eq i32 %b, 42
  %r = select i1 %c, i32 %b, i32 0
  ret i32 %r
}
