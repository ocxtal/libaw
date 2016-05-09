#! /usr/bin/env python
# encoding: utf-8

def options(opt):
	opt.recurse('zf')
	opt.load('compiler_c')

def configure(conf):
	conf.recurse('gaba')
	conf.recurse('gref')
	conf.recurse('zf')

	conf.load('ar')
	conf.load('compiler_c')

	conf.env.append_value('CFLAGS', '-O3')
	conf.env.append_value('CFLAGS', '-std=c99')
	conf.env.append_value('CFLAGS', '-march=native')

	conf.env.append_value('LIB_AW', conf.env.LIB_GABA + conf.env.LIB_GREF + conf.env.LIB_ZF)
	conf.env.append_value('DEFINES_AW', conf.env.DEFINES_GABA + conf.env.DEFINES_GREF + conf.env.DEFINES_ZF)
	conf.env.append_value('OBJ_AW', ['aw.o'] + conf.env.OBJ_GABA + conf.env.OBJ_GREF + conf.env.OBJ_ZF)


def build(bld):
	bld.recurse('gaba')
	bld.recurse('gref')
	bld.recurse('zf')

	bld.objects(source = 'aw.c', target = 'aw.o')

	bld.stlib(
		source = ['unittest.c'],
		target = 'aw',
		use = bld.env.OBJ_AW,
		lib = bld.env.LIB_AW,
		defines = bld.env.DEFINES_AW)

	bld.program(
		source = ['unittest.c'],
		target = 'unittest',
		use = bld.env.OBJ_AW,
		lib = bld.env.LIB_AW,
		defines = ['TEST'] + bld.env.DEFINES_AW)
