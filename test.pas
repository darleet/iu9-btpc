program Demo1;

var
  x, sum: integer;

procedure AddToSum(v: integer);
begin
  sum := sum + v;
end;

begin
  x := 1;
  sum := 0;

  while x <= 10 do
  begin
    if (x mod 2) = 0 then
      AddToSum(x);
    x := x + 1;
  end;

  writeln(sum);
end.
