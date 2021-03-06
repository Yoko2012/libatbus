﻿/**
 * @brief 所有channel文件的模式均为 c + channel<br />
 *        使用c的模式是为了简单、结构清晰并且避免异常<br />
 *        附带c++的部分是为了避免命名空间污染并且c++的跨平台适配更加简单
 */

#include <assert.h>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <stdint.h>
#include <utility>

#if (defined(__cplusplus) && __cplusplus >= 201103L) || (defined(_MSC_VER) && _MSC_VER >= 1800)
#include <type_traits>
#endif

#include "algorithm/murmur_hash.h"
#include "common/string_oprs.h"
#include "config/compile_optimize.h"


#include "detail/libatbus_config.h"
#include "detail/libatbus_error.h"
#include "lock/atomic_int_type.h"
#include "std/thread.h"

#define MEM_CHANNEL_NAME "ATBUSMEM"

namespace atbus {
    namespace channel {

        namespace detail {
            template <bool is_hash64>
            struct hash_factor;

            template <>
            struct hash_factor<false> {
                static uint32_t hash(uint32_t seed, const void *s, size_t l) {
                    return util::hash::murmur_hash3_x86_32(s, static_cast<int>(l), seed);
                    // CRC算法性能太差，包长度也不太可能超过2GB
                    // return atbus::detail::crc32(crc, static_cast<const unsigned char *>(s), l);
                }
            };

            template <>
            struct hash_factor<true> {
                static uint64_t hash(uint64_t seed, const void *s, size_t l) {
                    return util::hash::murmur_hash3_x86_32(s, static_cast<int>(l), static_cast<uint32_t>(seed));
                    // CRC算法性能太差，包长度也不太可能超过2GB
                    // return atbus::detail::crc64(crc, static_cast<const unsigned char *>(s), l);
                }
            };
        }

        typedef ATBUS_MACRO_DATA_ALIGN_TYPE data_align_type;

        // 配置数据结构
        typedef struct {
            size_t protect_node_count;
            size_t protect_memory_size;
            uint64_t conf_send_timeout_ms;

            size_t write_retry_times;
            // TODO 接收端校验号(用于保证只有一个接收者)
            volatile util::lock::atomic_int_type<size_t> atomic_recver_identify;
        } mem_conf;

        // 通道头
        typedef struct {
            char node_magic[8]; // 魔术串，用于标识数据类型

            // 数据节点
            size_t node_size;
            size_t node_size_bin_power; // (用于优化算法) node_size = 1 << node_size_bin_power
            size_t node_count;

            // [atomic_read_cur, atomic_write_cur) 内的数据块都是已使用的数据块
            // atomic_write_cur指向的数据块一定是空块，故而必然有一个node的空洞
            // c11的stdatomic.h在很多编译器不支持并且还有些潜规则(gcc 不能使用-fno-builtin 和 -march=xxx)，故而使用c++版本
            volatile util::lock::atomic_int_type<size_t> atomic_read_cur;  // util::lock::atomic_int_type也是POD类型
            volatile util::lock::atomic_int_type<size_t> atomic_write_cur; // util::lock::atomic_int_type也是POD类型

            // 第一次读到正在写入数据的时间
            uint64_t first_failed_writing_time;

            volatile util::lock::atomic_int_type<uint32_t> atomic_operation_seq; // 操作序列号(用于保证只有一个接收者)

            // 配置
            mem_conf conf;
            size_t area_channel_offset;
            size_t area_head_offset;
            size_t area_data_offset;
            size_t area_end_offset;

            // 统计信息
            size_t write_check_sequence_failed_count; // 写完后校验操作序号错误
            size_t write_retry_count;                 // 写操作内部重试次数

            size_t read_bad_node_count;                // 读到的错误数据节点数量
            size_t read_bad_block_count;               // 读到的错误数据块数量
            size_t read_write_timeout_count;           // 读到的写超时保护数量
            size_t read_check_block_size_failed_count; // 读到的数据块长度检查错误数量
            size_t read_check_node_size_failed_count;  // 读到的数据节点和长度检查错误数量
            size_t read_check_hash_failed_count;       // 读到的数据节点和长度检查错误数量
        } mem_channel;

#if (defined(__cplusplus) && __cplusplus >= 201103L) || (defined(_MSC_VER) && _MSC_VER >= 1800)
        static_assert(std::is_standard_layout<mem_channel>::value, "mem_channel must be a standard layout");
#endif

        // 对齐头
        typedef struct {
            mem_channel channel;
            char align[4 * 1024 - sizeof(mem_channel)]; // 对齐到4KB,用于以后拓展
        } mem_channel_head_align;


        // 数据节点头
        typedef struct {
            uint32_t flag;
            uint32_t operation_seq;
        } mem_node_head;

        // 数据头
        typedef struct {
            size_t buffer_size;
            data_align_type fast_check;
        } mem_block_head;


        typedef enum {
            MF_WRITEN = 0x00000001,
            MF_START_NODE = 0x00000002,
        } MEM_FLAG;

        namespace detail {
            /**
             * @brief 调试辅助信息
             * @note 不再使用tls记录调试信息，以防跨线程dump拿不到数据
             */
            static size_t last_action_channel_end_node_index = 0;
            static size_t last_action_channel_begin_node_index = 0;
            static mem_channel *last_action_channel_ptr = NULL;
        }

        /**
         * @brief 内存通道常量
         * @note 为了压缩内存占用空间，这里使用手动对齐，不直接用 #pragma pack(sizoef(long))
         */
        struct mem_block {
            enum size_def {
                channel_head_size = sizeof(mem_channel_head_align),
                block_head_size = ((sizeof(mem_block_head) - 1) / sizeof(data_align_type) + 1) * sizeof(data_align_type),
                node_head_size = ((sizeof(mem_node_head) - 1) / sizeof(data_align_type) + 1) * sizeof(data_align_type),

                node_data_size = ATBUS_MACRO_DATA_NODE_SIZE,
                node_head_data_size = node_data_size - block_head_size,
            };
        };

        /**
         * @brief 检测数字是2的几次幂
         */
        template <size_t S>
        struct mem_bin_power_check {
            static_assert(0 == (S & (S - 1)), "not 2^N"); // 必须是2的N次幂
            static_assert(S, "must not be 0");            // 必须大于0

            enum { value = mem_bin_power_check<(S >> 1)>::value + 1 };
        };

        template <>
        struct mem_bin_power_check<1> {
            enum { value = 0 };
        };

        /**
         * @brief 检查标记位
         * @param flag 待操作的flag
         * @param checked 检查项
         * @return 检查结果flag
         */
        static inline bool check_flag(uint32_t flag, MEM_FLAG checked) { return !!(flag & checked); }

        /**
         * @brief 设置标记位
         * @param flag 待操作的flag
         * @param checked 设置项flag
         * @return 设置结果flag
         */
        static inline uint32_t set_flag(uint32_t flag, MEM_FLAG checked) { return flag | checked; }

        /**
         * @brief 生存默认配置
         * @param conf
         */
        static void mem_default_conf(mem_channel *channel) {
            assert(channel);
            if (NULL == channel) {
                return;
            }

// 根据Jeffrey Dean大神2007年发布的一个数据，内存4ms大约能复制16MB数据
// 我们实测的每秒大约能传输数据量大于1GB，所以最大消息长度在4MB以内时4ms都足够传输整个消息，但是超出这个数值最好就再设置长一点
// 这里我们不考虑CPU调度切换，因为这个情况下无法估计时间，所以就让他超时吧
#if ATBUS_MACRO_MSG_LIMIT <= 4 * 1024 * 1024
            channel->conf.conf_send_timeout_ms = 4;
#else
            channel->conf.conf_send_timeout_ms = (ATBUS_MACRO_MSG_LIMIT / (1024 * 1024)) + 1;
#endif
            channel->conf.write_retry_times = 4; // 默认写序列错误重试4次

            // 默认留1/128的数据块用于保护缓冲区
            if (!channel->conf.protect_node_count && channel->conf.protect_memory_size) {
                channel->conf.protect_node_count =
                    (channel->conf.protect_memory_size + mem_block::node_data_size - 1) / mem_block::node_data_size;
            } else if (!channel->conf.protect_node_count) {
                channel->conf.protect_node_count = channel->node_count >> 7;

                // protect at most 16KB
                if (channel->conf.protect_node_count > ATBUS_MACRO_DATA_MAX_PROTECT_SIZE / mem_block::node_data_size) {
                    channel->conf.protect_node_count = ATBUS_MACRO_DATA_MAX_PROTECT_SIZE / mem_block::node_data_size;
                }
            }

            if (channel->conf.protect_node_count > channel->node_count) channel->conf.protect_node_count = channel->node_count;

            channel->conf.protect_memory_size = channel->conf.protect_node_count * mem_block::node_data_size;
        }

        /**
         * @brief 获取数据节点head
         * @param channel 内存通道
         * @param index 节点索引
         * @param data 数据区起始地址
         * @param data_len 到缓冲区末尾的长度
         * @return 节点head指针
         */
        static inline volatile mem_node_head *mem_get_node_head(mem_channel *channel, size_t index, void **data, size_t *data_len) {
            assert(channel);
            assert(index < channel->node_count);

            char *buf = (char *)channel;
            buf += channel->area_head_offset - channel->area_channel_offset;
            buf += index * mem_block::node_head_size;

            if (data || data_len) {
                char *data_ = (char *)channel + channel->area_data_offset - channel->area_channel_offset;
                data_ += index * mem_block::node_data_size;

                if (data) (*data) = (void *)data_;

                if (data_len) (*data_len) = channel->area_end_offset - channel->area_channel_offset + (char *)channel - data_;
            }

            return (volatile mem_node_head *)(void *)buf;
        }

        /**
         * @brief 获取数据块head
         * @param channel 内存通道
         * @param index 节点索引
         * @return 数据块head指针
         */
        static inline mem_block_head *mem_get_block_head(mem_channel *channel, size_t index, void **data, size_t *data_len) {
            assert(channel);
            assert(index < channel->node_count);

            char *buf = (char *)channel + channel->area_data_offset - channel->area_channel_offset;
            buf += index * mem_block::node_data_size;

            if (data) (*data) = (void *)(buf + mem_block::block_head_size);

            if (data_len)
                (*data_len) = channel->area_end_offset - channel->area_channel_offset + (char *)channel - buf - mem_block::block_head_size;

            return (mem_block_head *)(void *)buf;
        }

        /**
         * @brief 获取后面的数据块index
         * @param channel 内存通道
         * @param index 节点索引
         * @param offset 索引偏移
         * @return 数据块head指针
         */
        static inline size_t mem_next_index(mem_channel *channel, size_t index, size_t offset) {
            assert(channel);
            return (index + offset) % channel->node_count;
        }

        /**
         * @brief 获取可用的数据节点数量
         * @param channel 内存通道
         * @param read_cur 当前读游标
         * @param write_cur 当前写游标
         * @return 可用的节点数量
         */
        static inline size_t mem_get_available_node_count(mem_channel *channel, size_t read_cur, size_t write_cur) {
            assert(channel && channel->node_count);

            // 要留下一个node做tail, 所以多减1
            size_t ret = (read_cur + channel->node_count - write_cur - 1) % channel->node_count;
            if (ret >= channel->conf.protect_node_count) {
                ret -= channel->conf.protect_node_count;
            } else {
                ret = 0;
            }

            return ret;
        }

        /**
         * @brief 获取使用的数据节点数量
         * @param channel 内存通道
         * @param begin_cur 起始游标
         * @param end_cur 结束游标
         * @return 使用的数据节点数量
         */
        static inline size_t mem_get_node_range_count(mem_channel *channel, size_t begin_cur, size_t end_cur) {
            assert(channel && channel->node_count);
            return (end_cur + channel->node_count - begin_cur) % channel->node_count;
        }

        /**
         * @brief 获取前面的的数据块index
         * @param channel 内存通道
         * @param index 节点索引
         * @param offset 索引偏移
         * @return 数据块head指针
         */
        // static inline size_t mem_previous_index(mem_channel* channel, size_t index, size_t offset) {
        //    assert(channel);
        //    return (index + channel->node_count - offset) % channel->node_count;
        //}

        static inline uint32_t mem_fetch_operation_seq(mem_channel *channel) {
            uint32_t ret = ++channel->atomic_operation_seq;
            while (0 == ret) {
                ret = ++channel->atomic_operation_seq;
            }

            return ret;
        }

        /**
         * @brief 计算一定长度数据需要的数据node数量
         * @param len 数据长度
         * @return 数据长度需要的数据块数量
         */
        static inline size_t mem_calc_node_num(mem_channel *channel, size_t len) {
            assert(channel);
            // channel->node_size 必须是2的N次方，所以使用优化算法
            return (len + mem_block::block_head_size + channel->node_size - 1) >> channel->node_size_bin_power;
        }

        /**
         * @brief 生成校验码
         * @param src 源数据
         * @param len 数据长度
         * @note Hash 快速校验
         */
        static inline data_align_type mem_fast_check(const void *src, size_t len) {
            return static_cast<data_align_type>(detail::hash_factor<sizeof(data_align_type) >= sizeof(uint64_t)>::hash(0, src, len));
        }

        // 对齐单位的大小必须是2的N次方
        static_assert(0 == (sizeof(data_align_type) & (sizeof(data_align_type) - 1)), "data align size must be 2^N");
        // 节点大小必须是2的N次
        static_assert(0 == ((mem_block::node_data_size - 1) & mem_block::node_data_size), "node size must be 2^N");
        // 节点大小必须是对齐单位的2的N次方倍
        static_assert(0 == (mem_block::node_data_size & (mem_block::node_data_size - sizeof(data_align_type))),
                      "node size must be [data align size] * 2^N");


        int mem_attach(void *buf, size_t len, mem_channel **channel, const mem_conf *conf) {
            // 缓冲区最小长度为数据头+空洞node的长度
            if (len < sizeof(mem_channel_head_align) + mem_block::node_data_size + mem_block::node_head_size)
                return EN_ATBUS_ERR_CHANNEL_SIZE_TOO_SMALL;

            mem_channel_head_align *head = (mem_channel_head_align *)buf;
            if (channel) *channel = &head->channel;

            if (0 != UTIL_STRFUNC_STRNCASE_CMP(MEM_CHANNEL_NAME, head->channel.node_magic, strlen(MEM_CHANNEL_NAME))) {
                return EN_ATBUS_ERR_CHANNEL_BUFFER_INVALID;
            }

            return EN_ATBUS_ERR_SUCCESS;
        }

        int mem_init(void *buf, size_t len, mem_channel **channel, const mem_conf *conf) {
            // 缓冲区最小长度为数据头+空洞node的长度
            if (len < sizeof(mem_channel_head_align) + mem_block::node_data_size + mem_block::node_head_size)
                return EN_ATBUS_ERR_CHANNEL_SIZE_TOO_SMALL;

            memset(buf, 0x00, len);
            mem_channel_head_align *head = (mem_channel_head_align *)buf;

            // 节点计算
            head->channel.node_size = mem_block::node_data_size;
            {
                head->channel.node_size_bin_power = 0;
                size_t node_size = head->channel.node_size;
                while (node_size > 1) {
                    node_size >>= 1;
                    ++head->channel.node_size_bin_power;
                }
            }
            head->channel.node_count = (len - mem_block::channel_head_size) / (head->channel.node_size + mem_block::node_head_size);

            // 偏移位置计算
            head->channel.area_channel_offset = (char *)&head->channel - (char *)buf;
            head->channel.area_head_offset = sizeof(mem_channel_head_align);
            head->channel.area_data_offset = head->channel.area_head_offset + head->channel.node_count * mem_block::node_head_size;
            head->channel.area_end_offset = head->channel.area_data_offset + head->channel.node_count * head->channel.node_size;

            // 配置初始化
            if (NULL != conf) {
                memcpy(&head->channel.conf, conf, sizeof(*conf));
            } else {
                mem_default_conf(&head->channel);
            }

            // 输出
            if (channel) *channel = &head->channel;

#ifdef UTIL_STRFUNC_C11_SUPPORT
            static_assert(sizeof(head->channel.node_magic) >= (sizeof(MEM_CHANNEL_NAME) - 1), "magic text size error");

            memcpy_s(head->channel.node_magic, sizeof(head->channel.node_magic), MEM_CHANNEL_NAME, sizeof(MEM_CHANNEL_NAME) - 1);
#else
            memcpy(head->channel.node_magic, MEM_CHANNEL_NAME, sizeof(head->channel.node_magic));
#endif
            return EN_ATBUS_ERR_SUCCESS;
        }

        static int mem_send_real(mem_channel *channel, const void *buf, size_t len) {
            if (NULL == channel) return EN_ATBUS_ERR_PARAMS;

            if (0 == len) return EN_ATBUS_ERR_SUCCESS;

            size_t node_count = mem_calc_node_num(channel, len);
            // 要写入的数据比可用的缓冲区还大
            if (node_count >= channel->node_count - channel->conf.protect_node_count) {
                return EN_ATBUS_ERR_BUFF_LIMIT;
            }

            // 获取操作序号
            uint32_t opr_seq = mem_fetch_operation_seq(channel);

            // 游标操作
            size_t read_cur = 0;
            size_t new_write_cur, write_cur = channel->atomic_write_cur.load();

            while (true) {
                read_cur = channel->atomic_read_cur.load();
                // std::atomic_thread_fence(std::memory_order_seq_cst);

                // 要留下一个node做tail, 所以多减1
                size_t available_node = mem_get_available_node_count(channel, read_cur, write_cur);
                if (node_count > available_node) {
                    return EN_ATBUS_ERR_BUFF_LIMIT;
                }

                // 新的尾部node游标
                new_write_cur = mem_next_index(channel, write_cur, node_count);

                // @see http://en.cppreference.com/w/cpp/atomic/atomic/compare_exchange
                // CAS, 使用compare_exchange_weak可能低概率出现移动成功但是返回失败，然后导致有一个数据块被复写
                // 详见 https://github.com/owt5008137/libatbus/issues/4
                bool f = channel->atomic_write_cur.compare_exchange_strong(write_cur, new_write_cur);

                if (likely(f)) break;

                // 发现冲突原子操作失败则重试
            }
            detail::last_action_channel_begin_node_index = write_cur;
            detail::last_action_channel_end_node_index = new_write_cur;
            detail::last_action_channel_ptr = channel;

            // 数据缓冲区操作 - 初始化
            void *buffer_start = NULL;
            size_t buffer_len = 0;
            mem_block_head *block_head = mem_get_block_head(channel, write_cur, &buffer_start, &buffer_len);
            memset(block_head, 0x00, sizeof(mem_block_head));

            // 数据缓冲区操作 - 要写入的节点
            {
                block_head->buffer_size = 0;

                volatile mem_node_head *first_node_head = mem_get_node_head(channel, write_cur, NULL, NULL);
                first_node_head->flag = set_flag(0, MF_START_NODE);
                first_node_head->operation_seq = opr_seq;

                for (size_t i = mem_next_index(channel, write_cur, 1); i != new_write_cur; i = mem_next_index(channel, i, 1)) {
                    volatile mem_node_head *this_node_head = mem_get_node_head(channel, i, NULL, NULL);
                    assert((char *)this_node_head < (char *)channel + channel->area_data_offset);

                    // 写数据node出现冲突
                    // 写超时会导致this_node_head还是之前版本的数据，并不会被清空。所以不再恢复 operation_seq
                    // if (this_node_head->operation_seq) {
                    //     return EN_ATBUS_ERR_NODE_BAD_BLOCK_WSEQ_ID;
                    // }

                    this_node_head->flag = set_flag(0, MF_WRITEN);
                    this_node_head->operation_seq = opr_seq;
                }
            }
            block_head->buffer_size = len;

            // 数据写入
            // fast_memcpy
            // 数据有回绕
            if (new_write_cur && new_write_cur < write_cur) {
                size_t copy_len = len > buffer_len ? buffer_len : len;
                memcpy(buffer_start, buf, copy_len);

                // 回绕nodes
                mem_get_node_head(channel, 0, &buffer_start, NULL);
                memcpy(buffer_start, (const char *)buf + copy_len, len - copy_len);
            } else {
                memcpy(buffer_start, buf, len);
            }
            block_head->fast_check = mem_fast_check(buf, len);

            // 设置首node header，数据写完标记
            {
                // 设置屏障，强制内存刷入
                UTIL_LOCK_ATOMIC_THREAD_FENCE(util::lock::memory_order_acquire);

                volatile mem_node_head *first_node_head = mem_get_node_head(channel, write_cur, NULL, NULL);
                first_node_head->flag = set_flag(first_node_head->flag, MF_WRITEN);

                // 设置屏障，强制内存刷入
                UTIL_LOCK_ATOMIC_THREAD_FENCE(util::lock::memory_order_release);
                // 再检查一次，以防memcpy时发生写冲突
                if (opr_seq != first_node_head->operation_seq) {
                    ++channel->write_check_sequence_failed_count;
                    return EN_ATBUS_ERR_NODE_BAD_BLOCK_CSEQ_ID;
                }
            }

            return EN_ATBUS_ERR_SUCCESS;
        }

        int mem_send(mem_channel *channel, const void *buf, size_t len) {
            if (NULL == channel) return EN_ATBUS_ERR_PARAMS;

            int ret = 0;
            size_t left_try_times = channel->conf.write_retry_times;
            while (left_try_times-- > 0) {
                int ret = mem_send_real(channel, buf, len);

                // 原子操作序列冲突，重试
                if (EN_ATBUS_ERR_NODE_BAD_BLOCK_CSEQ_ID == ret || EN_ATBUS_ERR_NODE_BAD_BLOCK_WSEQ_ID == ret) {
                    ++channel->write_retry_count;
                    continue;
                }

                return ret;
            }

            return ret;
        }

        int mem_recv(mem_channel *channel, void *buf, size_t len, size_t *recv_size) {
            if (NULL == channel) return EN_ATBUS_ERR_PARAMS;

            int ret = EN_ATBUS_ERR_SUCCESS;

            void *buffer_start = NULL;
            size_t buffer_len = 0;
            mem_block_head *block_head = NULL;
            size_t read_begin_cur = channel->atomic_read_cur.load();
            const size_t ori_read_cur = read_begin_cur;
            size_t read_end_cur;
            size_t write_cur = channel->atomic_write_cur.load();
            // std::atomic_thread_fence(std::memory_order_seq_cst);

            uint32_t timeout_operation_seq = 0;

            while (true) {
                read_end_cur = read_begin_cur;

                if (read_begin_cur == write_cur) {
                    ret = ret ? ret : EN_ATBUS_ERR_NO_DATA;
                    break;
                }

                volatile mem_node_head *node_head = mem_get_node_head(channel, read_begin_cur, NULL, NULL);

                /**
                 * 这个时候，可能写出端处于移动了atomic_write_cur，但是还没有写出 MF_START_NODE 的情况。所以情况列举如下:
                 *   MF_START_NODE | MF_WRITEN: 数据块已写完
                 *   MF_WRITEN: 节点容错
                 *   MF_START_NODE: 是起始节点但是数据未写完（也可能是发送端在写出过程中崩溃）
                 *   0: 移动游标后尚未设置MF_START_NODE，这个出现概率非常低，但是也会出现。（也可能是发送端在写出过程中崩溃）
                 *
                 * 由于MF_START_NODE和0都是无法判定是没写完还是写出端崩溃的，所以都要走超时检测逻辑。
                 * 但是如果被判定超时并且写出端只写出了部分节点的的 MF_WRITEN 这时候剩下的节点的flag都会是0。
                 *   如果这些都通过超时机制判定，则最多可能等待 消息长度*超时判定时长/节点长度，默认设置是最少2秒钟。
                 *   所以这里需要特别处理下，当进入超时流程后，所有非 MF_START_NODE 并且operation_seq相等的节点也应该视为错误。
                 *   注意上面这个流程只能在超时流程中进行，因为其他错误流程可能第一个数据块错误，但是紧接着的第二个数据块处于正在写出的状态而没有设置
                 *   MF_START_NODE和operation_seq。我们的operation_seq取值范围是uint32，所以max(uint32)*节点长度（默认是500GB）以内的通道里operation_seq不会重复
                 *   我们的数据通道不可能使用这么大的内存，所以加上operation_seq后能尽可能地消除空数据快的超时影响
                 */
                // 容错处理 -- 未写入完成
                if (likely(check_flag(node_head->flag, MF_WRITEN))) {
                    // 容错处理 -- 不是起始节点
                    if (unlikely(!check_flag(node_head->flag, MF_START_NODE))) {
                        read_begin_cur = mem_next_index(channel, read_begin_cur, 1);
                        node_head->flag = 0;

                        ++channel->read_bad_node_count;
                        continue;
                    }

                    // 容错处理 -- 如果前面已经发现错误，这里就不能再消耗 MF_START_NODE了
                    // 防止后面把block弹出却没有读出数据并返回错误码
                    if (unlikely(ret)) {
                        break;
                    }

                } else {
                    uint64_t cnow = (uint64_t)(clock() / (CLOCKS_PER_SEC / 1000)); // 转换到毫秒

                    // 上面提到的快速跳过流程
                    if (unlikely(timeout_operation_seq && timeout_operation_seq == node_head->operation_seq &&
                                 !check_flag(node_head->flag, MF_START_NODE))) {
                        read_begin_cur = mem_next_index(channel, read_begin_cur, 1);
                        node_head->flag = 0;

                        ++channel->read_bad_node_count;
                        continue;
                    }

                    // 初次读取超时
                    if (!channel->first_failed_writing_time) {
                        channel->first_failed_writing_time = cnow;
                        ret = ret ? ret : EN_ATBUS_ERR_NO_DATA;
                        break;
                    }

                    uint64_t cd = cnow > channel->first_failed_writing_time ? cnow - channel->first_failed_writing_time
                                                                            : channel->first_failed_writing_time - cnow;
                    // 写入超时
                    if (channel->first_failed_writing_time && cd > channel->conf.conf_send_timeout_ms) {
                        timeout_operation_seq = node_head->operation_seq;

                        read_begin_cur = mem_next_index(channel, read_begin_cur, 1);
                        node_head->flag = 0;

                        ++channel->read_bad_node_count;
                        ++channel->read_bad_block_count;
                        ++channel->read_write_timeout_count;

                        channel->first_failed_writing_time = 0;
                        continue;
                    }

                    // 未到超时时间
                    ret = ret ? ret : EN_ATBUS_ERR_NO_DATA;
                    break;
                }

                // 数据检测
                block_head = mem_get_block_head(channel, read_begin_cur, &buffer_start, &buffer_len);

                // 缓冲区长度异常
                if (!block_head->buffer_size ||
                    block_head->buffer_size >= channel->area_end_offset - channel->area_data_offset - channel->conf.protect_memory_size) {
                    ret = ret ? ret : EN_ATBUS_ERR_NODE_BAD_BLOCK_BUFF_SIZE;

                    read_begin_cur = mem_next_index(channel, read_begin_cur, 1);
                    node_head->flag = 0;

                    ++channel->read_bad_node_count;
                    ++channel->read_check_block_size_failed_count;
                    continue;
                }

                // 写出的缓冲区不足
                if (block_head->buffer_size > len) {
                    ret = ret ? ret : EN_ATBUS_ERR_BUFF_LIMIT;
                    if (recv_size) *recv_size = block_head->buffer_size;

                    break;
                }


                // 重置操作码（防冲突+读检测）
                uint32_t check_opr_seq = node_head->operation_seq;
                for (read_end_cur = read_begin_cur; read_end_cur != write_cur; read_end_cur = mem_next_index(channel, read_end_cur, 1)) {
                    volatile mem_node_head *this_node_head = mem_get_node_head(channel, read_end_cur, NULL, NULL);
                    if (this_node_head->operation_seq != check_opr_seq) {
                        break;
                    }

                    // 如果出现异常了两个连续写入块有相同的operation_seq，会在这里被会切割开
                    if (read_end_cur != read_begin_cur && check_flag(this_node_head->flag, MF_START_NODE)) {
                        break;
                    }

                    // 如果前面触发了超时保护，则会有一批节点的operation_seq未被清空。为保证行为一致，所以这里也不再清空 operation_seq 了
                    // this_node_head->operation_seq = 0;
                    this_node_head->flag = 0;
                }

                // 有效的node数量检查
                {
                    size_t nodes_num = mem_get_node_range_count(channel, read_begin_cur, read_end_cur);
                    if (mem_calc_node_num(channel, block_head->buffer_size) != nodes_num) {
                        ret = ret ? ret : EN_ATBUS_ERR_NODE_BAD_BLOCK_NODE_NUM;
                        read_begin_cur = mem_next_index(channel, read_begin_cur, 1);
                        // 上面的循环已经重置过flag了

                        ++channel->read_bad_node_count;
                        ++channel->read_check_node_size_failed_count;
                        continue;
                    }
                }

                break;
            }


            do {
                // 出错退出, 移动读游标到最后读取位置
                if (ret) {
                    break;
                }

                channel->first_failed_writing_time = 0;

                // 接收数据 - 无回绕

                if (block_head->buffer_size <= buffer_len) {
                    memcpy(buf, buffer_start, block_head->buffer_size);

                } else { // 接收数据 - 有回绕
                    memcpy(buf, buffer_start, buffer_len);

                    // 回绕nodes
                    mem_get_node_head(channel, 0, &buffer_start, NULL);
                    memcpy((char *)buf + buffer_len, buffer_start, block_head->buffer_size - buffer_len);
                }
                data_align_type fast_check = mem_fast_check(buf, block_head->buffer_size);

                if (recv_size) *recv_size = block_head->buffer_size;

                // 校验不通过
                if (fast_check != block_head->fast_check) {
                    ++channel->read_check_hash_failed_count;
                    ret = ret ? ret : EN_ATBUS_ERR_BAD_DATA;
                }

            } while (false);

            // 设置游标
            if (ori_read_cur != read_end_cur) {
                // 设置屏障，保证这个执行前内存已被刷入
                UTIL_LOCK_ATOMIC_THREAD_FENCE(util::lock::memory_order_release);
                channel->atomic_read_cur.store(read_end_cur);
            }

            // 用于调试的节点编号信息
            detail::last_action_channel_begin_node_index = ori_read_cur;
            detail::last_action_channel_end_node_index = read_end_cur;
            detail::last_action_channel_ptr = channel;
            return ret;
        }

        std::pair<size_t, size_t> mem_last_action() {
            return std::make_pair(detail::last_action_channel_begin_node_index, detail::last_action_channel_end_node_index);
        }

        void mem_show_channel(mem_channel *channel, std::ostream &out, bool need_node_status, size_t need_node_data) {
            if (NULL == channel) {
                return;
            }

            size_t read_cur = channel->atomic_read_cur.load();
            size_t write_cur = channel->atomic_write_cur.load();
            size_t available_node = mem_get_available_node_count(channel, read_cur, write_cur);

            out << "Summary:" << std::endl
                << "\tchannel node size: " << channel->node_size << std::endl
                << "\tchannel node count: " << channel->node_count << std::endl
                << "\tchannel using memory size: " << (channel->area_end_offset - channel->area_channel_offset) << std::endl
                << "\tchannel available node number: " << available_node << std::endl
                << std::endl;

            out << "Configure:" << std::endl
                << "\tsend timeout(ms): " << channel->conf.conf_send_timeout_ms << std::endl
                << "\tprotect memory size(Bytes): " << channel->conf.protect_memory_size << std::endl
                << "\tprotect node number: " << channel->conf.protect_node_count << std::endl
                << "\twrite retry times: " << channel->conf.write_retry_times << std::endl
                << std::endl;

            out << "IO:" << std::endl
                << "\tfirst waiting time: " << channel->first_failed_writing_time << std::endl
                << "\tread index: " << read_cur << std::endl
                << "\twrite index: " << write_cur << std::endl
                << "\toperation sequence: " << channel->atomic_operation_seq << std::endl
                << std::endl;

            out << "Statistics:" << std::endl
                << "\twrite - check sequence failed: " << channel->write_check_sequence_failed_count << std::endl
                << "\twrite - retry times: " << channel->write_retry_count << std::endl
                << "\tread - bad node: " << channel->read_bad_node_count << std::endl
                << "\tread - bad block: " << channel->read_bad_block_count << std::endl
                << "\tread - write timeout: " << channel->read_write_timeout_count << std::endl
                << "\tread - check block size failed: " << channel->read_check_block_size_failed_count << std::endl
                << "\tread - check node count failed: " << channel->read_check_node_size_failed_count << std::endl
                << "\tread - check hash failed: " << channel->read_check_hash_failed_count << std::endl
                << std::endl;

            out << "Debug:" << std::endl
                << "\tlast action - channel: " << detail::last_action_channel_ptr << std::endl
                << "\tlast action - begin node index: " << detail::last_action_channel_begin_node_index << std::endl
                << "\tlast action - end node index: " << detail::last_action_channel_end_node_index << std::endl
                << std::endl;

            if (need_node_status) {
                out << std::endl << "Node head list:" << std::endl;
                for (size_t i = 0; i < channel->node_count; ++i) {
                    void *data_ptr = 0;
                    volatile mem_node_head *node_head = mem_get_node_head(channel, i, &data_ptr, NULL);
                    bool start_node = check_flag(node_head->flag, MF_START_NODE);

                    if (start_node) {
                        mem_block_head *block_head = mem_get_block_head(channel, i, NULL, NULL);
                        out << "Node index: " << std::setw(10) << i << " => seq=" << node_head->operation_seq << ", is start node=Yes"
                            << ", Data Length=" << block_head->buffer_size << ", Hash=" << block_head->fast_check
                            << ", is written=" << (check_flag(node_head->flag, MF_WRITEN) ? "Yes" : "No") << ", data(Hex): ";
                    } else {
                        out << "Node index: " << std::setw(10) << i << " => seq=" << node_head->operation_seq << ", is start node=No"
                            << ", is written=" << (check_flag(node_head->flag, MF_WRITEN) ? "Yes" : "No") << ", data(Hex): ";
                    }

                    if (need_node_data < mem_block::node_data_size) {
                        util::string::dumphex(data_ptr, need_node_data, out);
                    } else {
                        util::string::dumphex(data_ptr, mem_block::node_data_size, out);
                    }
                    out << std::endl;
                }

                out << "IO (after dump nodes):" << std::endl
                    << "\tfirst waiting time: " << channel->first_failed_writing_time << std::endl
                    << "\tread index: " << channel->atomic_read_cur << std::endl
                    << "\twrite index: " << channel->atomic_write_cur << std::endl
                    << "\toperation sequence: " << channel->atomic_operation_seq << std::endl
                    << std::endl;
            }
        }
    }
}
