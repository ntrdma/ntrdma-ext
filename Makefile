CFLAGS_MODULE := -Wall -Werror -DNTRDMA_FULL_ETH=1 -DNTRDMA_GIT_HASH=\"$(shell git --git-dir=$M/.git rev-list -1 --abbrev-commit HEAD)\"
obj-y += drivers/
