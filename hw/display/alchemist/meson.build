alchemist_ss = ss.source_set()
alchemist_ss.add(files(
  'alchemist.c',
  'alchemist_pcode.c',
  'alchemist_forcewake.c',
  'alchemist_vram.c',
))

system_ss.add_all(when: 'CONFIG_ALCHEMIST', if_true: alchemist_ss)
