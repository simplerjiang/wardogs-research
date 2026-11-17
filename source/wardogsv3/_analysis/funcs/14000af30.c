// procedure 0x14000af30  source=loader-memory-flow-rea.json  evidence=ev_e3aaf3cbc26b5448e8d5676d37f14c5531af8a8229f2475f7c29c64272481da3


void FUN_14000af30(undefined8 param_1)

{
  undefined1 auStack_48 [40];
  undefined1 local_20 [16];
  ulonglong local_10;
  
  local_10 = DAT_140062d80 ^ (ulonglong)auStack_48;
  (*(code *)0x4b758)(local_20,param_1);
  (*(code *)0x4b60a)(local_20);
  if ((local_10 ^ (ulonglong)auStack_48) == DAT_140062d80) {
    return;
  }
                    /* WARNING: Subroutine does not return */
  FUN_140014fe0();
}

