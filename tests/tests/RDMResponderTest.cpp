/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Library General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * RDMResponderTest.cpp
 * Tests for the RDMResponder code.
 * Copyright (C) 2015 Simon Newton
 */

#include <gtest/gtest.h>

#include <ola/rdm/UID.h>
#include <ola/rdm/RDMCommand.h>
#include <ola/rdm/RDMEnums.h>
#include <ola/rdm/RDMCommandSerializer.h>
#include <memory>

#include "rdm_responder.h"
#include "Array.h"
#include "Matchers.h"
#include "MessageHandlerMock.h"
#include "plib_ports_mock.h"

using ::testing::Args;
using ::testing::NiceMock;
using ::testing::StrictMock;
using ::testing::Return;
using ::testing::_;
using ola::rdm::UID;
using ola::rdm::NewDiscoveryUniqueBranchRequest;
using ola::rdm::NewMuteRequest;
using ola::rdm::NewUnMuteRequest;
using ola::rdm::RDMDiscoveryRequest;
using ola::rdm::RDMGetRequest;
using ola::rdm::RDMRequest;
using ola::rdm::RDMSetRequest;
using std::unique_ptr;

class MockSender {
 public:
  MOCK_METHOD3(SendResponse, void(bool include_break,
                                  const IOVec* data,
                                  unsigned int iov_count));
};

MockSender *g_sender = NULL;

void SendResponse(bool include_break,
                  const IOVec* data,
                  unsigned int iov_count) {
  if (g_sender) {
    g_sender->SendResponse(include_break, data, iov_count);
  }
}

bool UIDRequiresAction(const UID &uid) {
  uint8_t uid_data[UID_LENGTH];
  uid.Pack(uid_data, UID_LENGTH);
  return RDMResponder_UIDRequiresAction(uid_data);
}

// Tests for RDMResponder_VerifyChecksum
//-----------------------------------------------------------------------------
class ChecksumTest : public ::testing::TestWithParam<uint32_t> {
 public:
  static const uint8_t sample_message[];
};

const uint8_t ChecksumTest::sample_message[] = {
  0xcc, 0x01, 0x18, 0x7a, 0x70, 0x00, 0x00, 0x00, 0x00, 0x7a, 0x70, 0x12, 0x34,
  0x56, 0x78, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x02, 0x00, 0x03, 0xdf
};

TEST_P(ChecksumTest, checksumFails) {
  EXPECT_FALSE(RDMResponder_VerifyChecksum(sample_message, GetParam()));
}

INSTANTIATE_TEST_CASE_P(
    sizeTooSmall,
    ChecksumTest,
    ::testing::Range(
        0u,
        static_cast<unsigned int>(arraysize(ChecksumTest::sample_message) - 1))
);

TEST_F(ChecksumTest, checksumPasses) {
  EXPECT_TRUE(RDMResponder_VerifyChecksum(sample_message,
                                          arraysize(sample_message)));
}

TEST_F(ChecksumTest, checksumMismatch) {
  uint8_t bad_packet[arraysize(sample_message)];
  memcpy(bad_packet, sample_message, arraysize(sample_message));
  bad_packet[arraysize(sample_message) - 1]++;

  EXPECT_FALSE(RDMResponder_VerifyChecksum(bad_packet, arraysize(bad_packet)));
}

// Tests for the rest of the RDMResponder module.
//-----------------------------------------------------------------------------
class RDMResponderTest : public testing::Test {
 public:
  RDMResponderTest()
      : m_controller_uid(0x7a70, 0x10000000),
        m_our_uid(TEST_UID) {
  }

  void SetUp() {
    PLIB_PORTS_SetMock(&m_ports_mock);
    g_sender = &m_sender_mock;

    m_default_settings.identify_port = PORT_CHANNEL_D;
    m_default_settings.identify_bit = PORTS_BIT_POS_0;
    m_default_settings.mute_port = PORT_CHANNEL_D;
    m_default_settings.mute_bit = PORTS_BIT_POS_1;
    m_our_uid.Pack(reinterpret_cast<uint8_t*>(&m_default_settings.uid),
                   UID_LENGTH);

    RDMResponder_Initialize(&m_default_settings, SendResponse);
  }

  void TearDown() {
    PLIB_PORTS_SetMock(NULL);
    g_sender = NULL;
  }

  void SendRequest(const RDMRequest *request) {
    ola::io::ByteString data;
    data.push_back(RDM_START_CODE);
    ASSERT_TRUE(ola::rdm::RDMCommandSerializer::Pack(*request, &data));
    RDMResponder_HandleRequest(
        reinterpret_cast<const RDMHeader*>(data.data()),
        request->ParamData(),
        request->ParamDataSize());
  }

 protected:
  StrictMock<MockSender> m_sender_mock;
  NiceMock<MockPeripheralPorts> m_ports_mock;
  RDMResponderSettings m_default_settings;

  UID m_controller_uid;
  UID m_our_uid;

  static const uint8_t TEST_UID[UID_LENGTH];
};

const uint8_t RDMResponderTest::TEST_UID[] = {0x7a, 0x70, 1, 2, 3, 4};

TEST_F(RDMResponderTest, requiresAction) {
  RDMResponder_Initialize(&m_default_settings, nullptr);

  EXPECT_FALSE(UIDRequiresAction(UID(0, 0)));
  EXPECT_TRUE(UIDRequiresAction(UID::AllDevices()));
  EXPECT_TRUE(UIDRequiresAction(m_our_uid));
  EXPECT_TRUE(UIDRequiresAction(UID::VendorcastAddress(m_our_uid)));
  EXPECT_FALSE(UIDRequiresAction(UID::VendorcastAddress(0x7a7a)));
}

TEST_F(RDMResponderTest, invalidCommand) {
  const uint8_t expected_data[] = {
    0xcc, 0x01, 0x1a,
    0x7a, 0x70, 0x10, 0x00, 0x00, 0x00,  // dst UID
    0x7a, 0x70, 0x01, 0x02, 0x03, 0x04,  // src UID
    0x00, 0x02, 0x00, 0x00, 0x00,
    0x21, 0x1f, 0xff, 0x2,
    0x00, 0x00,
    0x04, 0x18
  };

  EXPECT_CALL(m_sender_mock,
              SendResponse(true, _, _))
     .With(Args<1, 2>(PayloadIs(expected_data, arraysize(expected_data))))
     .Times(1);

  // 0x1fff isn't a PID (yet!)
  unique_ptr<RDMRequest> request(new RDMGetRequest(
      m_controller_uid, m_our_uid, 0, 0, 0, 0x1fff, NULL, 0));
  SendRequest(request.get());
}

TEST_F(RDMResponderTest, discovery) {
  const uint8_t expected_data[] = {
    0xfe, 0xfe, 0xfe, 0xfe, 0xfe, 0xfe, 0xfe, 0xaa,
    0xfa, 0x7f, 0xfa, 0x75, 0xab, 0x55, 0xaa, 0x57,
    0xab, 0x57, 0xae, 0x55, 0xae, 0x57, 0xee, 0xff
  };

  EXPECT_CALL(m_sender_mock,
              SendResponse(false, _, 1))
     .With(Args<1, 2>(PayloadIs(expected_data, arraysize(expected_data))))
     .Times(4);

  unique_ptr<RDMDiscoveryRequest> request(NewDiscoveryUniqueBranchRequest(
      m_controller_uid, UID(0, 0), UID::AllDevices(), 0));
  SendRequest(request.get());

  request.reset(NewDiscoveryUniqueBranchRequest(
      m_controller_uid, m_our_uid, m_our_uid, 0));
  SendRequest(request.get());

  request.reset(NewDiscoveryUniqueBranchRequest(
      m_controller_uid, UID(m_our_uid.ManufacturerId(), 0),
      UID::AllDevices(), 0));
  SendRequest(request.get());

  request.reset(NewDiscoveryUniqueBranchRequest(
      m_controller_uid, UID(m_our_uid.ManufacturerId(), 0),
      UID::VendorcastAddress(m_our_uid), 0));
  SendRequest(request.get());

  // Check we don't respond if muted
  EXPECT_FALSE(RDMResponder_IsMuted());
  unique_ptr<RDMDiscoveryRequest> mute_request(NewMuteRequest(
      m_controller_uid, UID::AllDevices(), 0));
  SendRequest(mute_request.get());
  EXPECT_TRUE(RDMResponder_IsMuted());

  request.reset(NewDiscoveryUniqueBranchRequest(
      m_controller_uid, UID(0, 0), UID::AllDevices(), 0));
  SendRequest(request.get());
}

TEST_F(RDMResponderTest, testMute) {
  const uint8_t expected_data[] = {
    0xcc, 0x01, 26,
    0x7a, 0x70, 0x10, 0x00, 0x00, 0x00,  // dst UID
    0x7a, 0x70, 0x01, 0x02, 0x03, 0x04,  // src UID
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x11, 0x00, 0x02, 0x02,
    0x00, 0x00,
    0x02, 0xea
  };

  EXPECT_CALL(m_sender_mock, SendResponse(true, _, _))
     .With(Args<1, 2>(PayloadIs(expected_data, arraysize(expected_data))))
     .Times(1);
  EXPECT_CALL(m_ports_mock,
              PinClear(PORTS_ID_0, PORT_CHANNEL_D, PORTS_BIT_POS_1))
     .Times(3);

  EXPECT_FALSE(RDMResponder_IsMuted());
  unique_ptr<RDMDiscoveryRequest> request(NewMuteRequest(
      m_controller_uid, m_our_uid, 0));
  SendRequest(request.get());
  EXPECT_TRUE(RDMResponder_IsMuted());

  // Broadcasts should return no response
  request.reset(NewMuteRequest(m_controller_uid, UID::AllDevices(), 0));
  SendRequest(request.get());

  // Similarly vendorcasts shouldn't trigger a response
  request.reset(NewMuteRequest(
      m_controller_uid, UID::VendorcastAddress(m_our_uid), 0));
  SendRequest(request.get());
}

TEST_F(RDMResponderTest, testUnMute) {
  const uint8_t expected_data[] = {
    0xcc, 0x01, 26,
    0x7a, 0x70, 0x10, 0x00, 0x00, 0x00,  // dst UID
    0x7a, 0x70, 0x01, 0x02, 0x03, 0x04,  // src UID
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x11, 0x00, 0x03, 0x02,
    0x00, 0x00,
    0x02, 0xeb
  };

  // Send a broadcast mute first.
  unique_ptr<RDMDiscoveryRequest> request(NewMuteRequest(
      m_controller_uid, UID::AllDevices(), 0));
  SendRequest(request.get());
  EXPECT_TRUE(RDMResponder_IsMuted());

  EXPECT_CALL(m_sender_mock,
              SendResponse(true, _, _))
     .With(Args<1, 2>(PayloadIs(expected_data, arraysize(expected_data))))
     .Times(1);
  EXPECT_CALL(m_ports_mock,
              PinSet(PORTS_ID_0, PORT_CHANNEL_D, PORTS_BIT_POS_1))
     .Times(3);

  request.reset(NewUnMuteRequest(m_controller_uid, m_our_uid, 0));
  SendRequest(request.get());
  EXPECT_FALSE(RDMResponder_IsMuted());

  // Broadcasts should return no response
  request.reset(NewUnMuteRequest(m_controller_uid, UID::AllDevices(), 0));
  SendRequest(request.get());

  request.reset(NewUnMuteRequest(
      m_controller_uid, UID::VendorcastAddress(m_our_uid), 0));
  SendRequest(request.get());
}


TEST_F(RDMResponderTest, subdeviceNack) {
  const uint8_t expected_data[] = {
    0xcc, 0x01, 0x1a,
    0x7a, 0x70, 0x10, 0x00, 0x00, 0x00,  // dst UID
    0x7a, 0x70, 0x01, 0x02, 0x03, 0x04,  // src UID
    0x00, 0x02, 0x00, 0x00, 0x00,
    0x21, 0x00, 0x60, 0x2,
    0x00, 0x09,
    0x03, 0x63
  };

  EXPECT_CALL(m_sender_mock,
              SendResponse(true, _, _))
     .With(Args<1, 2>(PayloadIs(expected_data, arraysize(expected_data))))
     .Times(1);

  unique_ptr<RDMRequest> request(new RDMGetRequest(
      m_controller_uid, m_our_uid, 0, 0, 1, PID_DEVICE_INFO, NULL, 0));
  SendRequest(request.get());
}

TEST_F(RDMResponderTest, supportedParameters) {
  const uint8_t expected_data[] = {
    0xcc, 0x01, 0x1c,
    0x7a, 0x70, 0x10, 0x00, 0x00, 0x00,  // dst UID
    0x7a, 0x70, 0x01, 0x02, 0x03, 0x04,  // src UID
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x21, 0x00, 0x50, 0x4,
    0x00, 0x80, 0x0, 0x81,
    0x04, 0x4d
  };

  EXPECT_CALL(m_sender_mock,
              SendResponse(true, _, _))
     .With(Args<1, 2>(PayloadIs(expected_data, arraysize(expected_data))))
     .Times(1);

  unique_ptr<RDMRequest> request(new RDMGetRequest(
      m_controller_uid, m_our_uid, 0, 0, 0, PID_SUPPORTED_PARAMETERS, NULL, 0));
  SendRequest(request.get());
}

TEST_F(RDMResponderTest, deviceInfo) {
  const uint8_t expected_data[] = {
    0xcc, 0x01, 43,
    0x7a, 0x70, 0x10, 0x00, 0x00, 0x00,  // dst UID
    0x7a, 0x70, 0x01, 0x02, 0x03, 0x04,  // src UID
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x21, 0x00, 0x60, 0x13,
    0x1, 0, 0x1, 0, 0x71, 0x01,
    0, 0, 0, 0,
    0, 0, 0, 0, 0xff, 0xff,
    0, 0, 0,
    0x05, 0xec
  };

  EXPECT_CALL(m_sender_mock,
              SendResponse(true, _, _))
     .With(Args<1, 2>(PayloadIs(expected_data, arraysize(expected_data))))
     .Times(1);

  unique_ptr<RDMRequest> request(new RDMGetRequest(
      m_controller_uid, m_our_uid, 0, 0, 0, PID_DEVICE_INFO, NULL, 0));
  SendRequest(request.get());
}

TEST_F(RDMResponderTest, deviceModelDescription) {
  const uint8_t response[] = {
    0xcc, 0x01, 0x29,
    0x7a, 0x70, 0x10, 0x00, 0x00, 0x00,  // dst UID
    0x7a, 0x70, 0x01, 0x02, 0x03, 0x04,  // src UID
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x21, 0x00, 0x80, 0x11,
    'J', 'a', ' ', 'R', 'u', 'l', 'e', ' ',
    'R', 'e', 's', 'p', 'o', 'n', 'd', 'e', 'r',
    0x09, 0xcb
  };

  EXPECT_CALL(m_sender_mock,
              SendResponse(true, _, _))
     .With(Args<1, 2>(PayloadIs(response, arraysize(response))))
     .Times(1);

  unique_ptr<RDMRequest> request(new RDMGetRequest(
      m_controller_uid, m_our_uid, 0, 0, 0, PID_DEVICE_MODEL_DESCRIPTION,
      NULL, 0));
  SendRequest(request.get());
}

TEST_F(RDMResponderTest, manufacturerLabel) {
  const uint8_t response[] = {
    0xcc, 0x01, 0x2d,
    0x7a, 0x70, 0x10, 0x00, 0x00, 0x00,  // dst UID
    0x7a, 0x70, 0x01, 0x02, 0x03, 0x04,  // src UID
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x21, 0x00, 0x81, 0x15,
    'O', 'p', 'e', 'n', ' ', 'L', 'i', 'g',
    'h', 't', 'i', 'n', 'g', ' ', 'P', 'r', 'o',
    'j', 'e', 'c', 't',
    0x0b, 0x7e
  };

  EXPECT_CALL(m_sender_mock,
              SendResponse(true, _, _))
     .With(Args<1, 2>(PayloadIs(response, arraysize(response))))
     .Times(1);

  unique_ptr<RDMRequest> request(new RDMGetRequest(
      m_controller_uid, m_our_uid, 0, 0, 0, PID_MANUFACTURER_LABEL,
      NULL, 0));
  SendRequest(request.get());
}

TEST_F(RDMResponderTest, softwareVersionLabel) {
  const uint8_t response[] = {
    0xcc, 0x01, 29,
    0x7a, 0x70, 0x10, 0x00, 0x00, 0x00,  // dst UID
    0x7a, 0x70, 0x01, 0x02, 0x03, 0x04,  // src UID
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x21, 0x00, 0xc0, 0x05,
    'A', 'l', 'p', 'h', 'a',
    0x05, 0xa4
  };

  EXPECT_CALL(m_sender_mock,
              SendResponse(true, _, _))
     .With(Args<1, 2>(PayloadIs(response, arraysize(response))))
     .Times(1);

  unique_ptr<RDMRequest> request(new RDMGetRequest(
      m_controller_uid, m_our_uid, 0, 0, 0, PID_SOFTWARE_VERSION_LABEL,
      NULL, 0));
  SendRequest(request.get());
}

TEST_F(RDMResponderTest, identifyDevice) {
  const uint8_t response[] = {
    0xcc, 0x01, 25,
    0x7a, 0x70, 0x10, 0x00, 0x00, 0x00,  // dst UID
    0x7a, 0x70, 0x01, 0x02, 0x03, 0x04,  // src UID
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x21, 0x10, 0x00, 0x01, 0x00,
    0x03, 0x06,
  };

  EXPECT_CALL(m_sender_mock,
              SendResponse(true, _, _))
     .With(Args<1, 2>(PayloadIs(response, arraysize(response))))
     .Times(1);
  EXPECT_CALL(m_ports_mock,
              PinSet(PORTS_ID_0, PORT_CHANNEL_D, PORTS_BIT_POS_0))
     .Times(1);
  EXPECT_CALL(m_ports_mock,
              PinClear(PORTS_ID_0, PORT_CHANNEL_D, PORTS_BIT_POS_0))
     .Times(1);

  unique_ptr<RDMRequest> request(new RDMGetRequest(
      m_controller_uid, m_our_uid, 0, 0, 0, PID_IDENTIFY_DEVICE,
      NULL, 0));
  SendRequest(request.get());

  uint8_t identify_command = 1;
  request.reset(new RDMSetRequest(
      m_controller_uid, UID::AllDevices(), 0, 0, 0, PID_IDENTIFY_DEVICE,
      &identify_command, sizeof(identify_command)));
  SendRequest(request.get());

  identify_command = 0;
  request.reset(new RDMSetRequest(
      m_controller_uid, UID::AllDevices(), 0, 0, 0, PID_IDENTIFY_DEVICE,
      &identify_command, sizeof(identify_command)));
  SendRequest(request.get());
}