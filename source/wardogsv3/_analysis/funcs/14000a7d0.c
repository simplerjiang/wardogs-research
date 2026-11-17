// procedure 0x14000a7d0  source=FUNCTION-REVIEW-rea.json  evidence=ev_362fc6fdd0303352e1618f020551de386c5f9956140ead4279843228bb6aff78


undefined4 FUN_14000a7d0(void)

{
  char cVar1;
  int iVar2;
  int iVar3;
  undefined1 auStack_2a8 [32];
  undefined8 local_288;
  int *local_280;
  undefined4 local_278;
  undefined4 local_274;
  undefined4 local_270;
  int local_26c;
  undefined1 local_268 [16];
  undefined1 local_258 [528];
  ulonglong local_48;
  
  local_48 = DAT_140062d80 ^ (ulonglong)auStack_2a8;
  local_26c = 0x48d6988f;
  local_280 = &local_26c;
LAB_14000a862:
  while( true ) {
    while (local_26c < 0x4a0d5087) {
      if (local_26c < 0x176c170b) {
        if (local_26c == -0x7d35bb4a) {
          iVar3 = FUN_14000aa10(&DAT_140063ef0,local_258);
          iVar2 = 0x4a0d5087;
          if (-1 < iVar3) {
            iVar2 = 0x7068b6cf;
          }
          goto LAB_14000a85b;
        }
        if (local_26c == -0xa882fd5) {
          if ((local_48 ^ (ulonglong)auStack_2a8) != DAT_140062d80) {
                    /* WARNING: Subroutine does not return */
            FUN_140014fe0();
          }
          return local_274;
        }
      }
      else if (local_26c == 0x176c170b) {
        local_270 = local_278;
        *local_280 = -0xa882fd5;
        local_274 = local_278;
      }
      else if (local_26c == 0x48d6988f) {
        cVar1 = FUN_140008c30(L"C:\\Windows\\System32\\Drivers\\abce1.sys",&DAT_14004e220,0x5220);
        iVar2 = -0xa882fd5;
        if (cVar1 != '\0') {
          iVar2 = 0x7b1d84db;
        }
        *local_280 = iVar2;
        local_274 = 0;
      }
    }
    if (0x7068b6ce < local_26c) break;
    if (local_26c == 0x4a0d5087) {
      (*(code *)0x4afae)(L"C:\\Windows\\System32\\Drivers\\abce1.sys");
      *local_280 = 0x176c170b;
      local_278 = 0;
    }
    else if (local_26c == 0x4bb080cf) {
      (*(code *)0x4b758)(local_268,&DAT_140063ef0);
      iVar2 = (*(code *)0x4b60a)(local_268);
      iVar3 = 0x4a0d5087;
      if (-1 < iVar2) {
        iVar3 = 0x176c170b;
      }
      *local_280 = iVar3;
      local_278 = 1;
    }
  }
  if (local_26c == 0x7068b6cf) {
    cVar1 = FUN_1400099b0(L"C:\\Windows\\System32\\Drivers\\abce1.sys:\'",&DAT_140053440,0x84e8);
    iVar2 = 0x4a0d5087;
    if (cVar1 != '\0') {
      iVar2 = 0x4bb080cf;
    }
  }
  else {
    if (local_26c != 0x7b1d84db) goto LAB_14000a862;
    local_288 = 0;
    iVar3 = (*(code *)0x4b714)(L"C:\\Windows\\System32\\Drivers\\abce1.sys:\'",0x208,local_258,0);
    iVar2 = 0x4a0d5087;
    if (-1 < iVar3) {
      iVar2 = -0x7d35bb4a;
    }
  }
LAB_14000a85b:
  *local_280 = iVar2;
  goto LAB_14000a862;
}

