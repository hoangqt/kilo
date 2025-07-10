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

test:
	$(CC) -o tests/test_runner -DTEST_BUILD tests/test_runner.c tests/test_simple.c tests/test_syntax_highlighting.c tests/test_open_comment.c tests/test_row_operations.c tests/test_status_message.c kilo.c -Wall -W -pedantic -std=c99
	./tests/test_runner


.PHONY: kilo clean install lint format test
