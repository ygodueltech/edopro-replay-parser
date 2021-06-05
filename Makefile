.PHONY: build run help

all: default

default: help

build: ## meson and ninja
	meson build
	ninja -C build

buildrun: build ## build then run
	./build/yrp ./Trains_vs_Orcust_2.yrpX

help:
	@grep -E '^[a-zA-Z0-9_-]+:.*?## .*$$' $(MAKEFILE_LIST) | sort | awk 'BEGIN {FS = ":.*?## "}; {printf "\033[36m%-30s\033[0m %s\n", $$1, $$2}'
