all:  build/python_dict
build/python_dict: src/python_dict.c Makefile src/template.c
	gcc $(EXTRA_CFLAGS) -g -O2 -lutil -lpthread -ldl -lm -I../ -I../Include src/python_dict.c ../libpython2.7.a -o build/python_dict
