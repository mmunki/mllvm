define i32 @main() {
entry:
  %c = icmp sgt i32 7, 9
  %m = select i1 %c, i32 7, i32 9
  ret i32 %m
}
