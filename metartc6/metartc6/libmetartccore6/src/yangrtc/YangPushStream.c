﻿//
// Copyright (c) 2019-2022 yanggaofeng
//
#include <yangrtc/YangPushStream.h>

#include <yangrtc/YangPush.h>
#include <yangrtc/YangRtcRtcp.h>
//#include <yangrtc/YangBandwidth.h>


#include <yangrtp/YangRtpPacket.h>
#include <yangrtp/YangRtpConstant.h>
#include <yangrtp/YangRtpConstant.h>
#include <yangrtp/YangRtcpPsfbCommon.h>

#include <yangutil/sys/YangLog.h>

int32_t yang_rtcpush_cache_nack(YangRtcPushStream *pub,
		YangRtpPacket *pkt, char *p, int plen) {
    if (pub == NULL||pkt==NULL||p==NULL)
		return ERROR_RTC_PUBLISH;
	int32_t err = Yang_Ok;
	//uint16_t seq = pkt->header.sequence;

	return err;
}



int32_t yang_rtcpush_on_rtcp_rr(YangRtcContext *context,
		YangRtcPushStream *pub, YangRtcpCommon *rtcp) {
	int32_t err = Yang_Ok;
	if(rtcp->ssrc==pub->audioSsrc)
		return context->stats.on_recvRR(yangtrue,&context->stats.recvStats,&context->stats.sendStats,rtcp);

	if(rtcp->ssrc==pub->videoSsrc)
		return context->stats.on_recvRR(yangfalse,&context->stats.recvStats,&context->stats.sendStats,rtcp);
	return err;
}

int32_t yang_rtcpush_on_rtcp_xr(YangRtcContext *context,
		YangRtcPushStream *pub, YangRtcpCommon *rtcp) {
	int32_t err = Yang_Ok;

	return err;
}


int32_t yang_rtcpush_on_rtcp_nack(YangRtcContext *context,
		YangRtcPushStream *pub, YangRtcpCommon *rtcp) {

	return Yang_Ok;
}

int32_t yang_rtcpush_do_request_keyframe(YangRtcContext *context,
		uint32_t ssrc) {
	if (context == NULL)
		return ERROR_RTC_PUBLISH;
	int32_t err = Yang_Ok;
	yang_do_request_keyframe(context, ssrc);
	return err;
}

int32_t yang_rtcpush_on_rtcp_ps_feedback(YangRtcContext *context,
		YangRtcPushStream *pub, YangRtcpCommon *rtcp) {
	if (context == NULL || pub == NULL)
		return ERROR_RTC_PUBLISH;
	int32_t err = Yang_Ok;

	uint8_t fmt = rtcp->header.rc;
	switch (fmt) {
	case kPLI: {
		uint32_t ssrc = pub->videoSsrc;
		if (ssrc) {
			yang_rtcpush_do_request_keyframe(context, ssrc);
		}
		break;
	}
	case kSLI: {
		yang_info("sli");
		break;
	}
	case kRPSI: {
		yang_info("rpsi");
		break;
	}
	case kAFB: {
		yang_info("afb");
		break;
	}
	default: {
		return yang_error_wrap(ERROR_RTC_RTCP,
				"unknown payload specific feedback=%u", fmt);
	}
	}

	return err;
}

int32_t yang_rtcpush_on_rtcp(YangRtcContext *context,
		YangRtcPushStream *pub, YangRtcpCommon *rtcp) {
	if (context == NULL || pub == NULL)
		return ERROR_RTC_PUBLISH;

	if (YangRtcpType_rr == rtcp->header.type) {

		return yang_rtcpush_on_rtcp_rr(context, pub, rtcp);
	} else if (YangRtcpType_rtpfb == rtcp->header.type) {
		//currently rtpfb of nack will be handle by player. TWCC will be handled by YangRtcRtcpI

		return yang_rtcpush_on_rtcp_nack(context, pub, rtcp);
	} else if (YangRtcpType_psfb == rtcp->header.type) {
		return yang_rtcpush_on_rtcp_ps_feedback(context, pub, rtcp);
	} else if (YangRtcpType_xr == rtcp->header.type) {

		return yang_rtcpush_on_rtcp_xr(context, pub, rtcp);
	} else if (YangRtcpType_bye == rtcp->header.type) {
		// TODO: FIXME: process rtcp bye.
		return Yang_Ok;
	}
	return Yang_Ok;
}


int32_t yang_rtcpush_send_rtcp_sr(YangRtcContext *context, YangRtcPushStream* pub){

	if(yang_send_rtcp_sr(context,yangtrue,pub->audioSsrc)!=Yang_Ok){
		return yang_error_wrap(1,"send audio sr fail");
	}
	if(yang_send_rtcp_sr(context,yangfalse,pub->videoSsrc)!=Yang_Ok){
		return yang_error_wrap(1,"send video sr fail");
	}
	return Yang_Ok;
}

void yang_create_rtcpush(YangRtcPush *ppush, uint32_t audioSsrc,
		uint32_t videoSsrc) {
	if (ppush == NULL)		return;
	YangRtcPushStream *pub=(YangRtcPushStream*) yang_calloc(1,sizeof(YangRtcPushStream));
	ppush->pubStream=pub;
	pub->mw_msgs = 0;
	pub->realtime = 1;

	pub->audioSsrc = audioSsrc;
	pub->videoSsrc = videoSsrc;


	ppush->cache_nack = yang_rtcpush_cache_nack;
	ppush->on_rtcp_nack = yang_rtcpush_on_rtcp_nack;
	ppush->on_rtcp_xr = yang_rtcpush_on_rtcp_xr;
	ppush->on_rtcp_rr = yang_rtcpush_on_rtcp_rr;
	ppush->on_rtcp = yang_rtcpush_on_rtcp;
	ppush->on_rtcp_ps_feedback = yang_rtcpush_on_rtcp_ps_feedback;
	ppush->send_rtcp_sr=yang_rtcpush_send_rtcp_sr;

}
void yang_destroy_rtcpush(YangRtcPush *push) {
	if (push == NULL)	return;

	yang_free(push->pubStream);
}

