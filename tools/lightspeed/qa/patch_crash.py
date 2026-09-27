def rep(p, old, new):
    s = open(p, encoding='utf-8').read()
    assert old in s, (p, old[:70])
    s = s.replace(old, new, 1)
    open(p, 'w', encoding='utf-8', newline='\n').write(s)

rep(r'C:\lightspeed\darktable\src\common\darktable.c',
'''#else
  InitializeMagickEx(darktable.progname, MAGICK_OPT_NO_SIGNAL_HANDER, NULL);
#endif''',
'''#else
  InitializeMagickEx(darktable.progname, MAGICK_OPT_NO_SIGNAL_HANDER, NULL);
#ifdef _WIN32
  // GraphicsMagick still installs its own unhandled exception filter on
  // Windows, restore ours so that crashes produce a backtrace
  dt_set_signal_handlers();
#endif
#endif''')

rep(r'C:\lightspeed\darktable\src\common\system_signal_handling.c',
'''    wchar_t *wexception_message = g_utf8_to_utf16(exception_message, -1, NULL, NULL, NULL);
    MessageBoxW(0, wexception_message, L"Error!", MB_OK);''',
'''    wchar_t *wexception_message = g_utf8_to_utf16(exception_message, -1, NULL, NULL, NULL);
    // unattended runs (tests) write the backtrace without asking
    if(!g_getenv("LIGHTSPEED_NO_CRASH_DIALOG"))
      MessageBoxW(0, wexception_message, L"Error!", MB_OK);
    else
      g_printerr("%s", exception_message);''')
print('ok')
