declare void @print_i32(i32 %v)

define i32 @main() {
entry:
  br label %loop
loop:
  %i = phi i32 [ 1, %entry ], [ %next, %loop ]
  %s = phi i32 [ 0, %entry ], [ %sum, %loop ]
  %sum = add i32 %s, %i
  %next = add i32 %i, 1
  %c = icmp sle i32 %next, 100
  br i1 %c, label %loop, label %done
done:
  call void @print_i32(i32 %sum)
  ret i32 0
}
