CFLAGS_MODULE := -Wall -Werror -DNTRDMA_GIT_HASH=\"$(shell git --git-dir=$M/.git rev-list -1 --abbrev-commit HEAD)\"
obj-y += drivers/
