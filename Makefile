TARGET?=kilo

kilo: kilo.c kilo.h
ifeq ($(CI),true)
	$(CC) -o $(TARGET) kilo.c -Wall -W -pedantic -std=c99 -target $(TARGET)
else
	$(CC) -o $(TARGET) kilo.c -Wall -W -pedantic -std=c99
endif

clean:
	rm $(TARGET)

install:
	chmod +x $(TARGET)
	cp $(TARGET) /usr/local/bin

lint:
	clang-tidy kilo.c kilo.h -- -Wall -W -pedantic -std=c99

format:
	clang-format -i kilo.c kilo.h

.PHONY: kilo clean install lint format
