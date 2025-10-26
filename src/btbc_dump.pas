program btbc_dump;
{$mode objfpc}{$H+}

uses
  SysUtils;

type
  TBTBCHeader = packed record
    Magic: array[0..3] of char;     // 'B','T','B','C'
    Version: Word;
    Flags: Word;
    EntryCodeOff: LongWord;
    ConstOff, ConstSize: LongWord;
    TypeOff,  TypeSize:  LongWord;
    GDataSize: LongWord;
    FTabOff,  FTabSize:  LongWord;
    CodeOff,  CodeSize:  LongWord;
    DbgOff,   DbgSize:   LongWord;
  end;

  TBTBCFunc = packed record
    Id:         LongWord;
    NameIdx:    LongWord;
    Level:      Word;
    HasSL:      Word;
    LocalsSize: LongWord;
    ArgsBytes:  LongWord;
    CodeOff:    LongWord;
    CodeSize:   LongWord;
  end;

var
  F: file;
  H: TBTBCHeader;
  Funcs: array of TBTBCFunc;
  Code: array of Byte;
  FnCount, i: Integer;

const
  OpNames: array[0..255] of PChar = (
    // 0..43
    'OPAdd','OPNeg','OPMul','OPDivD','OPRemD','OPDiv2','OPRem2',
    'OPEqlI','OPNEqI','OPLssI','OPLeqI','OPGtrI','OPGEqI',
    'OPDupl','OPSwap','OPAndB','OPOrB','OPLoad','OPStore','OPHalt',
    'OPWrI','OPWrC','OPWrL','OPRdI','OPRdC','OPRdL','OPEOF','OPEOL',
    'OPLdC','OPLdA','OPLdLA','OPLdL','OPLdG','OPStL','OPStG',
    'OPMove','OPCopy','OPAddC','OPMulC','OPJmp','OPJZ','OPCall',
    'OPAdjS','OPExit',
    // reserved
    nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,
    nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,
    nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,
    nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,
    nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,
    nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,
    nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,
    nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,
    nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,
    nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,
    nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,
    nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,
    nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,nil,
    nil,nil,nil,nil
  );

function HasImm32(op: Byte): Boolean; inline;
begin
  // all opcodes >= 28 have imm32
  Result := op >= 28;
end;

procedure ReadExact(var f: file; var buf; len: SizeInt);
var
  r: SizeInt;
begin
  BlockRead(f, buf, len, r);
  if r <> len then begin
    Writeln('Read error');
    Halt(2);
  end;
end;

function ReadHeader(const path: string; out codeOff, codeSize: LongWord): Boolean;
var
  sz, n: SizeInt;
begin
  AssignFile(F, path);
  Reset(F, 1);
  sz := FileSize(F);
  if sz < SizeOf(H) then begin
    Writeln('File too small.');
    Exit(False);
  end;
  ReadExact(F, H, SizeOf(H));
  if (H.Magic[0] <> 'B') or (H.Magic[1] <> 'T') or
     (H.Magic[2] <> 'B') or (H.Magic[3] <> 'C') then begin
    Writeln('Bad magic.');
    Exit(False);
  end;
  Writeln('BTBC v', H.Version, '  GDataSize=', H.GDataSize);
  Writeln('EntryCodeOff=', H.EntryCodeOff);
  Writeln('Const: off=', H.ConstOff, ' size=', H.ConstSize);
  Writeln('Types: off=', H.TypeOff,  ' size=', H.TypeSize);
  Writeln('Funcs: off=', H.FTabOff,  ' size=', H.FTabSize);
  Writeln('Code : off=', H.CodeOff,  ' size=', H.CodeSize);
  // funcs
  if H.FTabSize mod SizeOf(TBTBCFunc) <> 0 then begin
    Writeln('FTabSize not multiple of record size.');
    Exit(False);
  end;
  FnCount := H.FTabSize div SizeOf(TBTBCFunc);
  SetLength(Funcs, FnCount);
  if FnCount > 0 then begin
    Seek(F, H.FTabOff);
    ReadExact(F, Funcs[0], H.FTabSize);
  end;
  // code
  codeOff := H.CodeOff; codeSize := H.CodeSize;
  if (codeOff + codeSize > LongWord(sz)) then begin
    Writeln('Code section out of file.');
    Exit(False);
  end;
  SetLength(Code, codeSize);
  Seek(F, codeOff);
  ReadExact(F, Code[0], codeSize);
  Result := True;
end;

procedure DumpFuncs;
var i: Integer;
begin
  if FnCount = 0 then begin
    Writeln('No functions.');
    Exit;
  end;
  Writeln('Functions (', FnCount, '):');
  for i := 0 to FnCount-1 do
    with Funcs[i] do begin
      Writeln(Format('#%d level=%d SL=%d loc=%d args=%d code=[%d..%d) size=%d',
        [Id, Level, HasSL, LocalsSize, ArgsBytes, CodeOff, CodeOff+CodeSize, CodeSize]));
    end;
end;

function NameOfOp(op: Byte): string;
begin
  if Assigned(OpNames[op]) then
    Result := OpNames[op]
  else
    Result := 'OP_'+IntToStr(op);
end;

procedure DisasmCode;
var
  pc: LongWord;
  op: Byte;
  imm: LongInt absolute op; // TODO: syntax highlights
  v: LongInt;
begin
  Writeln('Disassembly:');
  pc := 0;
  while pc < LongWord(Length(Code)) do begin
    op := Code[pc];
    if HasImm32(op) then begin
      if pc + 5 > LongWord(Length(Code)) then begin
        Writeln(Format('%8d: %-10s ??? <truncated>', [pc, NameOfOp(op)]));
        Break;
      end;
      Move(Code[pc+1], v, 4);
      Writeln(Format('%8d: %-10s %d', [pc, NameOfOp(op), v]));
      Inc(pc, 5);
    end else begin
      Writeln(Format('%8d: %-10s', [pc, NameOfOp(op)]));
      Inc(pc, 1);
    end;
  end;
end;

var
  codeOff, codeSize: LongWord;
begin
  if ParamCount < 1 then begin
    Writeln('Usage: btbc_dump <file.btbc>');
    Halt(1);
  end;
  if not ReadHeader(ParamStr(1), codeOff, codeSize) then Halt(2);
  Writeln;
  DumpFuncs;
  Writeln;
  DisasmCode;
end.
