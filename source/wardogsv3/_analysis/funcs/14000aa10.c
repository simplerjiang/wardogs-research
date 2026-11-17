// procedure 0x14000aa10  source=FUNCTION-REVIEW-rea.json  evidence=ev_83baa12ef437a3cd2c4987349468f106616166c9374a03007a519108ecd8ec28


int FUN_14000aa10(wchar_t *param_1,wchar_t *param_2)

{
  short sVar1;
  wchar_t wVar2;
  int iVar3;
  int iVar4;
  bool bVar5;
  undefined1 auStack_338 [32];
  wchar_t *local_318;
  int local_310;
  wchar_t local_302;
  int *local_300;
  wchar_t local_2f2;
  int local_2f0;
  int local_2ec;
  wchar_t *local_2e8;
  int local_2e0;
  int local_2dc;
  int local_2d8;
  int local_2d4;
  wchar_t *local_2d0;
  wchar_t *local_2c8;
  longlong local_2c0;
  wchar_t *local_2b8;
  wchar_t *local_2b0;
  wchar_t *local_2a8;
  longlong local_2a0;
  wchar_t *local_298;
  wchar_t *local_290;
  wchar_t *local_288;
  wchar_t *local_280;
  wchar_t *local_278;
  longlong local_270;
  int local_264;
  wchar_t local_260 [2];
  undefined4 local_25c;
  wchar_t local_258 [264];
  ulonglong local_48;
  
  local_48 = DAT_140062d80 ^ (ulonglong)auStack_338;
  wcscpy_s(param_1,0x35,L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\");
  local_264 = 0x45b93d12;
  local_300 = &local_264;
  local_2d0 = param_2;
  local_2c8 = param_2;
LAB_14000aaae:
  while( true ) {
    while (-0x74a2170 < local_264) {
      if (local_264 < 0x474a0112) {
        if (local_264 < 0x325f4abd) {
          if (local_264 == -0x74a216f) {
            local_290 = local_2e8;
            local_2c8 = local_2a8 + 1;
            *local_300 = 0x45b93d12;
            local_2d0 = local_2e8;
            local_288 = local_2c8;
          }
          else if (local_264 == 0x300aa048) {
            param_1[~((ulonglong)~(uint)local_2a0 | 0xffffffff00000000)] = L'\0';
            local_2ec = (*(code *)0x4b6bc)(0,param_1);
            iVar3 = 0x325f4abd;
            if (-1 < local_2ec) {
              iVar3 = 0x79f84780;
            }
            *local_300 = iVar3;
            local_2e0 = local_2ec;
          }
        }
        else if (local_264 == 0x45b93d12) {
          local_2b0 = local_2d0;
          local_2a8 = local_2c8;
          local_2f2 = *local_2c8;
          *local_300 = 0x474a0112;
        }
        else if (local_264 == 0x325f4abd) {
          if ((local_48 ^ (ulonglong)auStack_338) != DAT_140062d80) {
                    /* WARNING: Subroutine does not return */
            FUN_140014fe0();
          }
          return local_2ec;
        }
      }
      else if (local_264 < 0x65474f44) {
        if (local_264 == 0x474a0112) {
          bVar5 = SBORROW2(local_2f2,0x5c);
          sVar1 = local_2f2 + L'ﾤ';
          iVar3 = -0x36950739;
          iVar4 = -0x73229ad0;
          goto LAB_14000aeb7;
        }
        if (local_264 == 0x5b79da89) {
          wVar2 = local_302 + L'ￒ';
          goto LAB_14000aa9f;
        }
      }
      else if (local_264 == 0x65474f44) {
        local_2e8 = local_2a8 + 1;
        *local_300 = -0x74a216f;
        local_298 = local_2e8;
      }
      else if (local_264 == 0x79f84780) {
        local_25c = 1;
        wcscpy_s(local_258,5,L"\\??\\");
        FUN_14001c898(local_258,0x87,param_2);
        iVar3 = FUN_14001c978(local_258);
        local_310 = iVar3 * 2 + 2;
        local_318 = local_258;
        local_2f0 = (*(code *)0x4b78a)(0,param_1,L"ImagePath",1);
        iVar3 = -0x363a1b46;
        if (-1 < local_2f0) {
          iVar3 = -0x16986c72;
        }
        *local_300 = iVar3;
        local_2dc = local_2f0;
      }
    }
    if (-0x363a1b47 < local_264) break;
    if (local_264 < -0x53c919a2) {
      if (local_264 == -0x7fd2e8a7) {
        local_2b8 = local_280 + 1;
        local_2c0 = local_2a0 + 1;
        param_1[local_2a0] = local_302;
        *local_300 = -0x53c919a2;
        local_278 = local_2b8;
        local_270 = local_2c0;
      }
      else if (local_264 == -0x73229ad0) {
        iVar3 = -0x74a216f;
        if (local_2f2 == L'\0') {
          iVar3 = -0x53c919a2;
        }
        *local_300 = iVar3;
        local_2e8 = local_2b0;
        local_2c0 = 0x34;
        local_2b8 = local_2e8;
      }
    }
    else if (local_264 == -0x53c919a2) {
      local_2a0 = local_2c0;
      local_280 = local_2b8;
      local_302 = *local_2b8;
      *local_300 = -0x2fe11760;
    }
    else if (local_264 == -0x36950739) {
      iVar3 = -0x74a216f;
      if (local_2f2 == L'\\') {
        iVar3 = 0x65474f44;
      }
      *local_300 = iVar3;
      local_2e8 = local_2b0;
    }
  }
  if (local_264 < -0x26d47a4c) {
    if (local_264 == -0x363a1b46) {
      local_2d4 = local_2f0;
      *local_300 = 0x325f4abd;
      local_2ec = local_2f0;
      goto LAB_14000aaae;
    }
    if (local_264 != -0x2fe11760) goto LAB_14000aaae;
    bVar5 = SBORROW2(local_302,0x2e);
    sVar1 = local_302 + L'ￒ';
    iVar3 = 0x5b79da89;
    iVar4 = -0x26d47a4c;
LAB_14000aeb7:
    if (bVar5 != sVar1 < 0) {
      iVar3 = iVar4;
    }
  }
  else {
    wVar2 = local_302;
    if (local_264 != -0x26d47a4c) {
      if (local_264 == -0x16986c72) {
        local_318 = (wchar_t *)&local_25c;
        local_310 = 4;
        (*(code *)0x4b78a)(0,param_1,L"Type",4);
        local_260[0] = L'\x03';
        local_260[1] = L'\0';
        local_318 = local_260;
        local_310 = 4;
        local_2f0 = (*(code *)0x4b78a)(0,param_1,L"Start",4);
        *local_300 = -0x363a1b46;
        local_2d8 = local_2f0;
      }
      goto LAB_14000aaae;
    }
LAB_14000aa9f:
    iVar3 = -0x7fd2e8a7;
    if (wVar2 == L'\0') {
      iVar3 = 0x300aa048;
    }
  }
  *local_300 = iVar3;
  goto LAB_14000aaae;
}

