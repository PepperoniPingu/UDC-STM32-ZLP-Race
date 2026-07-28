/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal proof of concept for the Zephyr udc_stm32 endpoint busy-flag
 * race (drivers/usb/udc/udc_stm32.c). See README.md.
 *
 * handle_msg_data_in() clears the endpoint busy flag at function entry,
 * before it knows whether the transfer is actually finished. That flag is
 * the only thing stopping a concurrent udc_stm32_ep_enqueue() ->
 * udc_stm32_tx() from arming the hardware again. When the completed data
 * stage still needs a trailing ZLP, the handler arms the hardware a second
 * time and returns, so the endpoint looks idle while the hardware still
 * owns a transfer. An enqueue landing in that window freezes the endpoint.
 *
 * This class reproduces the trigger deliberately:
 *   - every bulk IN transfer is an exact multiple of the endpoint MPS and
 *     carries the ZLP flag, so every completion takes the vulnerable
 *     branch;
 *   - up to POC_OUTSTANDING buffers are kept queued, so the endpoint is
 *     re-armed back to back;
 *   - buffers are enqueued from an independent thread with randomized
 *     timing, decoupled from USB completion events, so enqueues sample all
 *     transfer phases, including the ZLP window.
 *
 * With a host reading continuously, an unpatched driver wedges within the
 * first few hundred transfers: the device logs WEDGED and the host reader
 * times out. With fix.patch applied the stream runs indefinitely.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/drivers/usb/udc.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(zlprace, LOG_LEVEL_INF);

#define POC_VID 0x2fe3
#define POC_PID 0xf00d

/* Transfers in flight at the UDC; must exceed 1 so that enqueues keep
 * flowing while a transfer is in progress.
 */
#define POC_OUTSTANDING       4
/* Transfer length in packets: any exact multiple of MPS works */
#define POC_PACKETS_PER_XFER  2
/* Upper bound of the random enqueue jitter (microseconds) */
#define POC_JITTER_US         200

struct poc_desc {
	struct usb_if_descriptor if0;
	struct usb_ep_descriptor if0_in_ep;
	struct usb_ep_descriptor if0_hs_in_ep;
	struct usb_desc_header nil_desc;
};

static struct poc_desc poc_desc = {
	.if0 = {
		.bLength = sizeof(struct usb_if_descriptor),
		.bDescriptorType = USB_DESC_INTERFACE,
		.bInterfaceNumber = 0,
		.bAlternateSetting = 0,
		.bNumEndpoints = 1,
		.bInterfaceClass = USB_BCC_VENDOR,
		.bInterfaceSubClass = 0,
		.bInterfaceProtocol = 0,
		.iInterface = 0,
	},
	.if0_in_ep = {
		.bLength = sizeof(struct usb_ep_descriptor),
		.bDescriptorType = USB_DESC_ENDPOINT,
		.bEndpointAddress = 0x81,
		.bmAttributes = USB_EP_TYPE_BULK,
		.wMaxPacketSize = sys_cpu_to_le16(64U),
		.bInterval = 0x00,
	},
	.if0_hs_in_ep = {
		.bLength = sizeof(struct usb_ep_descriptor),
		.bDescriptorType = USB_DESC_ENDPOINT,
		.bEndpointAddress = 0x81,
		.bmAttributes = USB_EP_TYPE_BULK,
		.wMaxPacketSize = sys_cpu_to_le16(512U),
		.bInterval = 0x00,
	},
	.nil_desc = {
		.bLength = 0,
		.bDescriptorType = 0,
	},
};

static const struct usb_desc_header *poc_fs_desc[] = {
	(struct usb_desc_header *)&poc_desc.if0,
	(struct usb_desc_header *)&poc_desc.if0_in_ep,
	(struct usb_desc_header *)&poc_desc.nil_desc,
};

static const struct usb_desc_header *poc_hs_desc[] = {
	(struct usb_desc_header *)&poc_desc.if0,
	(struct usb_desc_header *)&poc_desc.if0_hs_in_ep,
	(struct usb_desc_header *)&poc_desc.nil_desc,
};

static atomic_t poc_enabled;
static atomic_t poc_done_cnt;
static uint16_t poc_mps;
static K_SEM_DEFINE(poc_slots, POC_OUTSTANDING, POC_OUTSTANDING);

static int poc_request(struct usbd_class_data *const c_data,
		       struct net_buf *const buf, const int err)
{
	net_buf_unref(buf);

	if (err != 0 && err != -ECONNABORTED) {
		LOG_ERR("Transfer failed (%d)", err);
	} else if (err == 0) {
		atomic_inc(&poc_done_cnt);
	}

	k_sem_give(&poc_slots);

	return 0;
}

static void *poc_get_desc(struct usbd_class_data *const c_data,
			  const enum usbd_speed speed)
{
	if (USBD_SUPPORTS_HIGH_SPEED && speed == USBD_SPEED_HS) {
		return poc_hs_desc;
	}

	return poc_fs_desc;
}

static void poc_enable(struct usbd_class_data *const c_data)
{
	struct usbd_context *ctx = usbd_class_get_ctx(c_data);

	if (usbd_bus_speed(ctx) == USBD_SPEED_HS) {
		poc_mps = 512U;
	} else {
		poc_mps = 64U;
	}

	LOG_INF("Enabled, bulk IN 0x81 mps=%u, transfer=%u+ZLP",
		poc_mps, POC_PACKETS_PER_XFER * poc_mps);
	atomic_set(&poc_enabled, 1);
}

static void poc_disable(struct usbd_class_data *const c_data)
{
	atomic_set(&poc_enabled, 0);
	LOG_INF("Disabled");
}

static int poc_init(struct usbd_class_data *c_data)
{
	return 0;
}

static struct usbd_class_api poc_api = {
	.request = poc_request,
	.get_desc = poc_get_desc,
	.enable = poc_enable,
	.disable = poc_disable,
	.init = poc_init,
};

static uint8_t poc_dummy; /* usbd_class_get_private() must not be needed */

USBD_DEFINE_CLASS(zlprace_0, &poc_api, &poc_dummy, NULL);

/* Cheap deterministic jitter; quality is irrelevant, decorrelation from
 * USB completion timing is what matters.
 */
static uint32_t poc_rand(void)
{
	static uint32_t s = 0x12345678;

	s = s * 1664525U + 1013904223U;
	return s ^ k_cycle_get_32();
}

static void poc_producer(void *a, void *b, void *c)
{
	while (true) {
		if (!atomic_get(&poc_enabled)) {
			k_sleep(K_MSEC(100));
			continue;
		}

		k_sem_take(&poc_slots, K_FOREVER);

		/* Sample a random point in the transfer phase of the
		 * previous buffers, including the ZLP window.
		 */
		k_busy_wait(poc_rand() % POC_JITTER_US);

		const uint16_t len = POC_PACKETS_PER_XFER * poc_mps;
		struct net_buf *buf;

		buf = usbd_ep_buf_alloc(&zlprace_0, 0x81, len);
		if (buf == NULL) {
			k_sem_give(&poc_slots);
			k_sleep(K_MSEC(10));
			continue;
		}

		memset(net_buf_add(buf, len), 0x5a, len);
		/* Exact-MPS-multiple transfer: terminate with a ZLP,
		 * forcing every completion through the vulnerable ZLP
		 * branch.
		 */
		udc_ep_buf_set_zlp(buf);

		if (usbd_ep_enqueue(&zlprace_0, buf) != 0) {
			net_buf_unref(buf);
			k_sem_give(&poc_slots);
			k_sleep(K_MSEC(10));
		}
	}
}

K_THREAD_DEFINE(poc_producer_tid, 2048, poc_producer, NULL, NULL, NULL,
		K_PRIO_PREEMPT(5), 0, 0);

/* Minimal usbd context setup */
USBD_DEVICE_DEFINE(poc_usbd, DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)),
		   POC_VID, POC_PID);
USBD_DESC_LANG_DEFINE(poc_lang);
USBD_DESC_MANUFACTURER_DEFINE(poc_mfr, "Zephyr PoC");
USBD_DESC_PRODUCT_DEFINE(poc_product, "udc_stm32 ZLP race PoC");
USBD_DESC_CONFIG_DEFINE(poc_fs_cfg_desc, "FS Configuration");
USBD_DESC_CONFIG_DEFINE(poc_hs_cfg_desc, "HS Configuration");
USBD_CONFIGURATION_DEFINE(poc_fs_config, 0, 250, &poc_fs_cfg_desc);
USBD_CONFIGURATION_DEFINE(poc_hs_config, 0, 250, &poc_hs_cfg_desc);

static int poc_usbd_add_speed(const enum usbd_speed speed,
			      struct usbd_config_node *config)
{
	int err;

	err = usbd_add_configuration(&poc_usbd, speed, config);
	if (err != 0) {
		return err;
	}

	return usbd_register_all_classes(&poc_usbd, speed, 1, NULL);
}

int main(void)
{
	uint32_t idle_seconds = 0;
	int err;

	err = usbd_add_descriptor(&poc_usbd, &poc_lang);
	err |= usbd_add_descriptor(&poc_usbd, &poc_mfr);
	err |= usbd_add_descriptor(&poc_usbd, &poc_product);
	if (err != 0) {
		LOG_ERR("Failed to add string descriptors");
		return err;
	}

	if (USBD_SUPPORTS_HIGH_SPEED &&
	    usbd_caps_speed(&poc_usbd) == USBD_SPEED_HS) {
		err = poc_usbd_add_speed(USBD_SPEED_HS, &poc_hs_config);
		if (err != 0) {
			return err;
		}
	}

	err = poc_usbd_add_speed(USBD_SPEED_FS, &poc_fs_config);
	if (err != 0) {
		return err;
	}

	err = usbd_init(&poc_usbd);
	if (err != 0) {
		LOG_ERR("usbd_init failed (%d)", err);
		return err;
	}

	err = usbd_enable(&poc_usbd);
	if (err != 0) {
		LOG_ERR("usbd_enable failed (%d)", err);
		return err;
	}

	LOG_INF("udc_stm32 ZLP race PoC ready, waiting for host reader");

	while (true) {
		k_sleep(K_SECONDS(1));

		uint32_t n = (uint32_t)atomic_set(&poc_done_cnt, 0);

		if (!atomic_get(&poc_enabled)) {
			idle_seconds = 0;
			continue;
		}

		LOG_INF("xfers/s=%u", n);

		if (n == 0) {
			idle_seconds++;
		} else {
			idle_seconds = 0;
		}

		/* The host reader polls continuously: sustained silence
		 * while enabled means the endpoint has wedged.
		 */
		if (idle_seconds == 3) {
			LOG_ERR("WEDGED: bulk IN produced no completions for 3 s");
		}
	}

	return 0;
}
