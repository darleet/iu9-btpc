program btvm;
{$mode objfpc}{$H+}

uses
  SysUtils, Math;

type
  TInt = LongInt;
  TWord32 = LongWord;
  PByte = ^Byte;
  PBTBCFunc = ^TBTBCFunc;

  TBTBCHeader = packed record
    Magic: array[0..3] of char;
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

  TFrame = record
    SavedPC: LongWord;
    SavedFP: LongWord;
    RetSlotOff: LongWord;
  end;

const
  STACK_MAX = 1 shl 20;  // 1 MB
  DATA_STACK_MAX = 1 shl 16; // 64К * 4 bytes

var
  H: TBTBCHeader;
  Funcs: array of TBTBCFunc;
  Code: array of Byte;

  // stack for int32/addr
  DS: array[0..DATA_STACK_MAX-1] of TInt;
  DSP: Integer = 0;

  Mem: array of Byte;
  FP: LongWord = 0; // Frame
  StackMemTop: LongWord = 0;

  CallStack: array[0..8191] of TFrame;
  CallSP: Integer = 0;

function HasImm32(op: Byte): Boolean; inline;
begin
  Result := op >= 28; // just because, docs
end;

function ReadLE32(const buf: array of Byte; ofs: LongWord): LongInt; inline;
begin
  Move(buf[ofs], Result, 4);
end;

procedure Push(v: TInt); inline;
begin
  if DSP >= DATA_STACK_MAX then begin
    Writeln('Data stack overflow'); Halt(10);
  end;
  DS[DSP] := v; Inc(DSP);
end;

function Pop: TInt; inline;
begin
  Dec(DSP);
  if DSP < 0 then begin
    Writeln('Data stack underflow'); Halt(11);
  end;
  Result := DS[DSP];
end;

procedure MemEnsure(size: LongWord);
begin
  if size > LongWord(Length(Mem)) then
    SetLength(Mem, size);
end;

procedure MemWrite32(off: LongWord; v: LongInt); inline;
begin
  MemEnsure(off+4);
  Move(v, Mem[off], 4);
end;

function MemRead32(off: LongWord): LongInt; inline;
begin
  if off+4 > LongWord(Length(Mem)) then begin
    Writeln('Memory read OOB at ', off);
    Halt(12);
  end;
  Move(Mem[off], Result, 4);
end;

procedure MemCopy(dst, src: LongWord; size: LongWord; overlap: Boolean);
begin
  MemEnsure(Max(dst+size, src+size));
  // AFAIK in any case we use same System.move
  if overlap then
    System.Move(Mem[src], Mem[dst], size)
  else
    System.Move(Mem[src], Mem[dst], size);
end;

procedure LoadBTBC(const path: string);
var
  F: file;
  sz: SizeInt;
begin
  AssignFile(F, path);
  Reset(F, 1);
  sz := FileSize(F);
  if sz < SizeOf(H) then begin Writeln('Too small'); Halt(2); end;
  BlockRead(F, H, SizeOf(H));
  if (H.Magic[0] <> 'B') or (H.Magic[1] <> 'T') or (H.Magic[2] <> 'B') or (H.Magic[3] <> 'C') then begin
    Writeln('Bad magic'); Halt(2);
  end;

  // funcs
  if H.FTabSize mod SizeOf(TBTBCFunc) <> 0 then begin
    Writeln('Bad FTabSize'); Halt(2);
  end;
  SetLength(Funcs, H.FTabSize div SizeOf(TBTBCFunc));
  if Length(Funcs) > 0 then begin
    Seek(F, H.FTabOff);
    BlockRead(F, Funcs[0], H.FTabSize);
  end;

  // code
  if (H.CodeOff + H.CodeSize > LongWord(sz)) then begin
    Writeln('Code OOB'); Halt(2);
  end;
  SetLength(Code, H.CodeSize);
  Seek(F, H.CodeOff);
  BlockRead(F, Code[0], H.CodeSize);

  CloseFile(F);

  // memory
  SetLength(Mem, H.GDataSize);
  FillChar(Mem[0], H.GDataSize, 0);
  FP := H.GDataSize;         // frames right after globals
  StackMemTop := FP;
end;

function FuncById(id: LongWord): PBTBCFunc;
begin
  if id >= LongWord(Length(Funcs)) then begin
    Writeln('CALL unknown func id=', id); Halt(20);
  end;
  Result := @Funcs[id];
end;

procedure VMRun;
var
  pc: LongWord;
  op: Byte;
  imm: LongInt;
  a,b: LongInt;
  addr, size, dst, src: LongWord;
  fn: ^TBTBCFunc;
  L, S, N: LongInt;
  retVal: LongInt;

  function NextImm32: LongInt; inline;
  begin
    Result := ReadLE32(Code, pc); Inc(pc, 4);
  end;

  procedure EnterFrame(localsSize, argsBytes: LongWord; hasSL: Boolean; poppedSL: LongInt);
  var total: LongWord;
      slOfs, baseArgs: LongWord;
      j: LongInt;
  begin
    // Frame: [locals L][retaddr][SL?][argN..arg1][return_slot]
    L := localsSize;
    S := IfThen(hasSL, 4, 0);

    if (argsBytes and 3) <> 0 then begin
      Writeln('argsBytes is not 4-byte aligned'); Halt(123);
    end;
  
    N := argsBytes div 4;

    total := L + 4 + S + argsBytes + 4;  // +retaddr +return_slot

    FP := StackMemTop;
    StackMemTop := StackMemTop + total;
    MemEnsure(StackMemTop);

    if L > 0 then FillChar(Mem[FP], L, 0);

    if hasSL then begin
      slOfs := FP + L + 4;
      MemWrite32(slOfs, poppedSL);
    end;

    // DS -> memory
    baseArgs := FP + L + 4 + S;
    for j := 0 to N-1 do begin
      a := Pop;
      MemWrite32(baseArgs + 4 * (j+1), a);
    end;

    // return_slot:
    MemWrite32(FP + L + 4 + S + argsBytes, 0);

    // remember where to put res and where to continue
    CallStack[CallSP].SavedPC := pc;
    CallStack[CallSP].SavedFP := FP;
    CallStack[CallSP].RetSlotOff := FP + L + 4 + S + argsBytes;
    Inc(CallSP);
  end;

  procedure LeaveFrameAndPushResult;
  begin
    Dec(CallSP);
    // return_slot -> op stack
    retVal := MemRead32(CallStack[CallSP].RetSlotOff);
    Push(retVal);
    pc := CallStack[CallSP].SavedPC;
    StackMemTop := CallStack[CallSP].SavedFP;
    FP := StackMemTop; // reset FP
  end;

var ch: char;
begin
  pc := H.EntryCodeOff;

  while pc < LongWord(Length(Code)) do begin
    op := Code[pc]; Inc(pc);

    case op of
      // math
      0  {OPAdd}:   begin b:=Pop; a:=Pop; Push(a+b); end;
      1  {OPNeg}:   begin a:=Pop; Push(-a); end;
      2  {OPMul}:   begin b:=Pop; a:=Pop; Push(a*b); end;
      3  {OPDivD}:  begin b:=Pop; a:=Pop; if b=0 then Halt(100); Push(a div b); end;
      4  {OPRemD}:  begin b:=Pop; a:=Pop; if b=0 then Halt(101); Push(a mod b); end;
      5  {OPDiv2}:  begin a:=Pop; Push(a div 2); end;
      6  {OPRem2}:  begin a:=Pop; Push(a mod 2); end;

      7  {OPEqlI}:  begin b:=Pop; a:=Pop; Push(ord(a=b)); end;
      8  {OPNEqI}:  begin b:=Pop; a:=Pop; Push(ord(a<>b)); end;
      9  {OPLssI}:  begin b:=Pop; a:=Pop; Push(ord(a<b)); end;
      10 {OPLeqI}:  begin b:=Pop; a:=Pop; Push(ord(a<=b)); end;
      11 {OPGtrI}:  begin b:=Pop; a:=Pop; Push(ord(a>b)); end;
      12 {OPGEqI}:  begin b:=Pop; a:=Pop; Push(ord(a>=b)); end;

      13 {OPDupl}:  begin a:=Pop; Push(a); Push(a); end;
      14 {OPSwap}:  begin b:=Pop; a:=Pop; Push(b); Push(a); end;

      15 {OPAndB}:  begin b:=Pop; a:=Pop; Push(a and b); end;
      16 {OPOrB}:   begin b:=Pop; a:=Pop; Push(a or b); end;

      // memory
      17 {OPLoad}:  begin addr:=LongWord(Pop); Push(MemRead32(addr)); end;
      18 {OPStore}: begin addr:=LongWord(Pop); a:=Pop; MemWrite32(addr, a); end;

      // input/output
      19 {OPHalt}:  begin if DSP>0 then ExitCode := Pop else ExitCode := 0; Halt(ExitCode); end;
      20 {OPWrI}:   begin Write(Pop); end;
      21 {OPWrC}:   begin ch := Chr(Byte(Pop)); Write(ch); end;
      22 {OPWrL}:   begin Writeln; end;
      23 {OPRdI}:   begin Read(a); Push(a); end;
      24 {OPRdC}:   begin Read(ch); Push(Ord(ch)); end;
      25 {OPRdL}:   begin ReadLn; end;
      26 {OPEOF}:   begin Push(ord(Eof)); end;
      27 {OPEOL}:   begin Push(ord(Eoln)); end;

      // addresses
      28 {OPLdC}:   begin imm := NextImm32; Push(imm); end;
      29 {OPLdA}:   begin imm := NextImm32; Push(imm); end;
      30 {OPLdLA}:  begin imm := NextImm32; Push(FP + LongWord(imm)); end;
      31 {OPLdL}:   begin imm := NextImm32; Push(MemRead32(FP + LongWord(imm))); end;
      32 {OPLdG}:   begin imm := NextImm32; Push(MemRead32(LongWord(imm))); end;
      33 {OPStL}:   begin imm := NextImm32; a := Pop; MemWrite32(FP + LongWord(imm), a); end;
      34 {OPStG}:   begin imm := NextImm32; a := Pop; MemWrite32(LongWord(imm), a); end;

      35 {OPMove}:  begin size := LongWord(NextImm32); dst := LongWord(Pop); src := LongWord(Pop); MemCopy(dst, src, size, True); end;
      36 {OPCopy}:  begin size := LongWord(NextImm32); dst := LongWord(Pop); src := LongWord(Pop); MemCopy(dst, src, size, False); end;

      37 {OPAddC}:  begin imm := NextImm32; a := Pop; Push(a + imm); end;
      38 {OPMulC}:  begin imm := NextImm32; a := Pop; Push(a * imm); end;

      39 {OPJmp}:   begin imm := NextImm32; pc := pc + LongWord(imm); end;
      40 {OPJZ}:    begin imm := NextImm32; a := Pop; if a = 0 then pc := pc + LongWord(imm); end;

      41 {OPCall}:  begin
        imm := NextImm32; // func_id
        fn := FuncById(LongWord(imm));
        if fn^.HasSL <> 0 then
          a := Pop // static link
        else
          a := 0;
        EnterFrame(fn^.LocalsSize, fn^.ArgsBytes, fn^.HasSL <> 0, a);
        // move to begin of func
        CallStack[CallSP-1].SavedPC := pc;
        pc := fn^.CodeOff;
      end;

      42 {OPAdjS}:  begin imm := NextImm32; FP := FP + LongWord(imm); end; // ENTER/LEAVE
      43 {OPExit}:  begin
        imm := NextImm32; // bytes to pop (args+SL)
        LeaveFrameAndPushResult;
      end;

    else
      Writeln('Unknown opcode: ', op, ' at pc=', pc-1);
      Halt(99);
    end;
  end;
end;

var
  path: string;
begin
  if ParamCount < 1 then begin
    Writeln('Usage: btvm <file.btbc>');
    Halt(1);
  end;
  path := ParamStr(1);
  LoadBTBC(path);
  VMRun;
end.
