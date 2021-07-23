#!/bin/bash

modprobe rdma_cm rdma_rxe

rdma link add rxe_0 type rxe netdev ens33

ibv_devices

