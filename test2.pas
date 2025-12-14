program Test2;
var
  x, y, sum: integer;
begin
  x := 10;
  y := 20;
  sum := x + y;
  writeln('x = ', x);
  writeln('y = ', y);
  writeln('sum = ', sum);
  
  if sum > 25 then
    writeln('Sum is greater than 25')
  else
    writeln('Sum is not greater than 25');
end.
