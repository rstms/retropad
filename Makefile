CC=gcc
WINDRES = windres
CFLAGS = -Wall -Wextra -Wno-unused-parameter -DUNICODE -D_UNICODE -mwindows
LDFLAGS = -mwindows -municode
LIBS = -luser32 -lgdi32 -lcomdlg32 -lcomctl32 -lshell32

OBJS = retropad.o file_io.o retropad.res.o

PROGRAM = retropad
COMPANY = retropad
VERSION != cat VERSION
MAJOR != awk <VERSION -F. '{print $$1}'
MINOR != awk <VERSION -F. '{print $$2}'
PATCH != awk <VERSION -F. '{print $$3}'
INSTALLER = $(PROGRAM)-v$(VERSION)-win32.exe

MACROS = \
  -D__VERSION__=$(VERSION) \
  -D__MAJOR__=$(MAJOR) \
  -D__MINOR__=$(MINOR) \
  -D__PATCH__=$(PATCH) \
  -D__PROGRAM__=$(PROGRAM) \
  -D__COMPANY__=$(COMPANY) \
  -D__DESCRIPTION__='heirloom notepad.exe clone' \
  -D__EXE_FILE__=$(PROGRAM).exe \
  -D__ICON_FILE__=$(PROGRAM).ico \
  -D__INSTALLER_FILE__=$(INSTALLER) \
  -D__ABOUT_URL__='https://github.com/rstms/retropad' \
  -D__FORK_URL__='https://github.com/PlummersSoftwareLLC/retropad' \
  -D__INSTALL_SIZE__=8192


$(INSTALLER): retropad.nsi retropad.exe LICENSE.txt
	makensis $<

LICENSE.txt: LICENSE
	unix2dos -n $< $@

retropad.nsi: retropad.nsi.in
	m4 <$< >$@ $(MACROS)

retropad.exe: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

retropad.o: retropad.c resource.h file_io.h
	$(CC) $(CFLAGS) -c retropad.c

file_io.o: file_io.c file_io.h resource.h
	$(CC) $(CFLAGS) -c file_io.c

retropad.rc: retropad.rc.in
	m4 <$< >$@ $(MACROS)

retropad.res.o: retropad.rc resource.h retropad.ico
	$(WINDRES) -i $(PWD)/$< -o $@

clean:
	rm -f $(OBJS) *.exe LICENSE.txt

sterile: clean
	rm -f retropad.rc retropad.nsi

bump: clean
	$(if $(shell git status --porcelain),$(error git status is dirty),)
	@echo >VERSION "$(MAJOR).$(MINOR).$(shell echo $$(($(PATCH) + 1)))"
	git add VERSION
	git commit -m "v$(shell cat VERSION)"
	git tag v$(shell cat VERSION)
	git push 
	git push origin v$(shell cat VERSION)

release: $(INSTALLER)
	$(if $(shell git status --porcelain),$(error git status is dirty),)
	./git-release

