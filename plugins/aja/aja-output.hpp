#pragma once

#include "aja-props.hpp"

#include <ajantv2/includes/ntv2testpatterngen.h>

#include <ajabase/common/types.h>
#include <ajabase/system/thread.h>

// #define AJA_WRITE_DEBUG_WAV
#ifdef AJA_WRITE_DEBUG_WAV
#include <ajabase/common/wavewriter.h>
#endif

#include <deque>
#include <memory>
#include <mutex>

struct VideoFrame {
	struct video_data frame;
	size_t size;
};


using VideoQueue = std::deque<VideoFrame>;

class CNTV2Card; // forward decl

class AJAOutput {
public:
	enum {
		// min queue sizes computed in AJAOutput
		kVideoQueueMaxSize = 15,
	};

	AJAOutput(CNTV2Card *card, const std::string &cardID,
		  const std::string &outputID, UWord deviceIndex,
		  const NTV2DeviceID deviceID);

	~AJAOutput();

	CNTV2Card *GetCard();

	void Initialize(const OutputProps &props);

	void SetOBSOutput(obs_output_t *output);
	obs_output_t *GetOBSOutput();

	void SetOutputProps(const OutputProps &props);
	OutputProps GetOutputProps() const;

	void CacheConnections(const NTV2XptConnections &cnx);
	void ClearConnections();

	void GenerateTestPattern(NTV2VideoFormat vf, NTV2PixelFormat pf,
				 NTV2TestPatternSelect pattern);

	void QueueVideoFrame(struct video_data *frame, size_t size);
	void ClearVideoQueue();
	size_t VideoQueueSize();

	void DMAVideoFromQueue();

	void CreateThread(bool enable = false);
	void StopThread();
	bool ThreadRunning();
	static void OutputThread(AJAThread *thread, void *ctx);

	std::string mCardID;
	std::string mOutputID;
	UWord mDeviceIndex;
	NTV2DeviceID mDeviceID;

	uint32_t mNumCardFrames;
	uint32_t mFirstCardFrame;
	uint32_t mLastCardFrame;
	uint32_t mWriteCardFrame;
	uint32_t mPlayCardFrame;
	uint32_t mPlayCardNext;
	uint32_t mFrameRateNum;
	uint32_t mFrameRateDen;

	uint64_t mVideoQueueFrames;
	uint64_t mVideoWriteFrames;
	uint64_t mVideoPlayFrames;

private:
	void calculate_card_frame_indices(uint32_t numFrames, NTV2DeviceID id,
					  NTV2Channel channel,
					  NTV2VideoFormat vf,
					  NTV2PixelFormat pf);

	uint32_t get_frame_count();

	CNTV2Card *mCard;

	OutputProps mOutputProps;

	NTV2TestPatternBuffer mTestPattern;

	bool mIsRunning;

	AJAThread mRunThread;
	mutable std::mutex mVideoLock;
	mutable std::mutex mRunThreadLock;

	std::unique_ptr<VideoQueue> mVideoQueue;

	obs_output_t *mOBSOutput;

	NTV2XptConnections mCrosspoints;
};
