TARGET?=kilo

kilo: kilo.c
	$(CC) -o $(TARGET) kilo.c -Wall -W -pedantic -std=c99 -target $(TARGET)

clean:
	rm $(TARGET)

install:
	chmod +x $(TARGET)
	cp $(TARGET) /usr/local/bin

lint:
	clang-tidy kilo.c -- -Wall -W -pedantic -std=c99

format:
	clang-format -i kilo.c

.PHONY: kilo clean install lint format
