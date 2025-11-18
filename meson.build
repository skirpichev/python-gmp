project('gmp', 'c',
        version: run_command('python', '-m', 'setuptools_scm',
                             check: true).stdout().strip())
py = import('python').find_installation(pure: false)
libzz = dependency('zz', version: '>= 0.6.0')
py.extension_module('gmp', ['fmt.c', 'gmp.c', 'utils.c'],
                    install: true, dependencies: libzz)
