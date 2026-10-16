CFLAGS := -O1 -g -Wall -Wextra -fsanitize=address,undefined

host: host.c ../basic.c ../basic.h ../store.c ../store.h
	gcc $(CFLAGS) host.c ../basic.c ../store.c -o host

test: host
	@fail=0; for t in *.bas; do \
	  n=$${t%.bas}; ./host < $$t > $$n.got 2>&1; \
	  if diff -u $$n.out $$n.got > /dev/null; then echo "OK    $$n"; \
	  else echo "FALLA $$n"; diff -u $$n.out $$n.got | head -20; fail=1; fi; \
	done; exit $$fail

clean:
	rm -f host *.got

.PHONY: test clean
