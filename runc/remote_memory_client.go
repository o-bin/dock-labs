package main

import (
	"fmt"

	"github.com/urfave/cli"
	"github.com/opencontainers/runc/libcontainer"
	"github.com/opencontainers/runc/libcontainer/configs"
)

var remoteMemoryClientCommand = cli.Command{
	Name:  "remote-memory-client",
	Usage: "internal command to handle remote memory (userfaultfd)",
	Flags: []cli.Flag{
		cli.StringFlag{
			Name:  "address",
			Usage: "address of the memory server",
		},
		cli.Int64Flag{
			Name:  "size",
			Usage: "size of the memory pool in bytes",
		},
		cli.StringFlag{
			Name:  "compression",
			Usage: "compression algorithm (e.g., lz4)",
		},
	},
	Action: func(context *cli.Context) error {
		addr := context.String("address")
		size := context.Int64("size")
		comp := context.String("compression")

		if addr == "" || size == 0 {
			return fmt.Errorf("address and size are required")
		}

		cfg := &configs.RemoteMemory{
			ServerAddress: addr,
			RAM:           size,
			Compression:   comp,
		}

		return libcontainer.RunRemoteMemoryClient(cfg)
	},
}
