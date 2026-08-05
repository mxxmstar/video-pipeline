#pragma once

/**
 * @file mediaflow.h
 * @brief MediaFlow 核心公共头文件。
 *
 * 业务模块通常只需包含本文件即可获得 Graph、节点基类、端口、Transport
 * 和 Executor。更细粒度的库代码可以直接包含 core 下的单个头文件。
 */

#include "mediaflow/core/executor.h"
#include "mediaflow/core/graph.h"
#include "mediaflow/core/node.h"
#include "mediaflow/core/port.h"
#include "mediaflow/core/transport.h"
#include "mediaflow/core/types.h"
#include "mediaflow/media_nodes.h"
