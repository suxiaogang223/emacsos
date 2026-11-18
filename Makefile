.DEFAULT_GOAL := build
.PHONY: build run test test-tooling check-web lint check test-guest web web-assets web-serve test-browser wasm help
.NOTPARALLEL: check

build:
	python3 scripts/machine.py build

run: build
	python3 scripts/machine.py run

test: test-tooling check-web

test-tooling:
	python3 -m unittest discover -s tests/tooling -p 'test_*.py'

check-web:
	python3 -m unittest discover -s tests/web -p 'test_*.py'

lint:
	python3 scripts/check-contribution.py --require-clean-submodule

test-guest: build
	python3 tests/guest/check.py

check: test lint test-guest

WEB_HOST ?= 127.0.0.1
WEB_PORT ?= 18082
WEB_URL ?= http://$(WEB_HOST):$(WEB_PORT)/
web-assets:
	python3 scripts/fetch-web-assets.py

wasm:
	python3 scripts/build-wasm.py

web: build web-assets
	python3 scripts/serve-web.py --host $(WEB_HOST) --port $(WEB_PORT)

web-serve: web-assets
	python3 scripts/serve-web.py --host $(WEB_HOST) --port $(WEB_PORT)

test-browser:
	EMACSOS_PLAYWRIGHT="$${EMACSOS_PLAYWRIGHT:-$(CURDIR)/build/web-browser-tools/node_modules/playwright}" node tests/web/browser.cjs '$(WEB_URL)'

help:
	@echo 'make build - build the pinned Linux and Emacs system'
	@echo 'make run - launch the native console with its retained data disk'
	@echo 'make web - build and serve the browser computer'
	@echo 'make wasm - rebuild the browser emulator and firmware from source'
	@echo 'make test - run host tooling and web tests'
	@echo 'make check - run host checks and two-boot native acceptance'
	@echo 'make test-browser - exercise an already running browser console'
