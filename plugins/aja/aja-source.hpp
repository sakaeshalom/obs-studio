#pragma once

#include "aja-props.hpp"

#include <obs-module.h>

#include <ajantv2/includes/ajatypes.h>

#include <ajabase/common/types.h>
#include <ajabase/system/thread.h>

#include <mutex>

class CNTV2Card;

class AJASource {
public:
	explicit AJASource(obs_source_t *source);

	~AJASource();

	void SetCard(CNTV2Card *card);
	CNTV2Card *GetCard();

	void SetOBSSource(obs_source_t *source);
	obs_source_t *GetOBSSource(void) const;
	void SetName(const std::string &name);
	std::string GetName() const;

	// Capture Thread stuff
	static void CaptureThread(AJAThread *thread, void *data);
	void Activate(bool enable = false);
	void Deactivate();
	bool IsCapturing() const;
	void SetCapturing(bool capturing);

	// CardEntry/Device stuff
	std::string CardID() const;
	void SetCardID(const std::string &cardID);
	uint32_t DeviceIndex() const;
	void SetDeviceIndex(uint32_t index);

	// Source Props
	void SetSourceProps(const SourceProps &props);
	SourceProps GetSourceProps() const;

	void CacheConnections(const NTV2XptConnections &cnx);
	void ClearConnections();

	bool ReadChannelVPIDs(NTV2Channel channel, VPIDData &vpids);

	bool ReadWireFormats(NTV2DeviceID device_id, IOSelection io_select,
			     NTV2VideoFormat &vf, NTV2PixelFormat &pf,
			     VPIDDataList &vpids);

	void ResetVideoBuffer(NTV2VideoFormat vf, NTV2PixelFormat pf);

	NTV2_POINTER mVideoBuffer;

private:
	CNTV2Card *mCard;

	std::string mSourceName;
	std::string mCardID;
	UWord mDeviceIndex;

	bool mBuffering;
	bool mIsCapturing;

	SourceProps mSourceProps;

	AJAThread *mCaptureThread;
	std::mutex mMutex;

	obs_source_t *mSource;

	NTV2XptConnections mCrosspoints;
};
