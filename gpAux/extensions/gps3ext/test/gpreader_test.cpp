#include "gpreader.cpp"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "mock_classes.h"

using ::testing::AtLeast;
using ::testing::Return;
using ::testing::Invoke;
using ::testing::Throw;
using ::testing::_;

class MockGPReader : public GPReader {
   public:
    MockGPReader(const S3Params& params, const string& urlWithOptions,
                 S3RESTfulService* mockRESTfulService)
        : GPReader(params, urlWithOptions) {
        restfulServicePtr = mockRESTfulService;
    }

    S3BucketReader& getBucketReader() {
        return this->bucketReader;
    }
};

class GPReaderTest : public testing::Test {
   protected:
    virtual void SetUp() {
        InitConfig(this->params, "data/s3test.conf", "default");
    }
    virtual void TearDown() {
    }

    S3Params params;
};

TEST_F(GPReaderTest, Construct) {
    string url = "s3://s3-us-west-2.amazonaws.com/s3test.pivotal.io/dataset1/normal";
    GPReader gpreader(this->params, url);

    EXPECT_EQ("secret_test", this->params.getCred().secret);
    EXPECT_EQ("accessid_test", this->params.getCred().accessID);
    EXPECT_EQ("ABCDEFGabcdefg", this->params.getCred().token);
}

TEST_F(GPReaderTest, Open) {
    string url = "s3://s3-us-west-2.amazonaws.com/s3test.pivotal.io/dataset1/normal";
    MockS3RESTfulService mockRESTfulService(this->params);
    MockGPReader gpreader(this->params, url, &mockRESTfulService);

    XMLGenerator generator;
    XMLGenerator* gen = &generator;
    gen->setName("s3test.pivotal.io")
        ->setPrefix("threebytes/")
        ->setIsTruncated(false)
        ->pushBuckentContent(BucketContent("threebytes/", 0))
        ->pushBuckentContent(BucketContent("threebytes/threebytes", 3));

    Response response(RESPONSE_OK, gen->toXML());

    EXPECT_CALL(mockRESTfulService, get(_, _)).WillOnce(Return(response));

    gpreader.open(this->params);

    const ListBucketResult& keyList = gpreader.getBucketReader().getKeyList();
    EXPECT_EQ((uint64_t)1, keyList.contents.size());
    EXPECT_EQ("threebytes/threebytes", keyList.contents[0].getName());
    EXPECT_EQ((uint64_t)3, keyList.contents[0].getSize());
}

TEST_F(GPReaderTest, Close) {
    string url = "s3://s3-us-west-2.amazonaws.com/s3test.pivotal.io/dataset1/normal";
    MockS3RESTfulService mockRESTfulService(this->params);
    MockGPReader gpreader(this->params, url, &mockRESTfulService);

    XMLGenerator generator;
    XMLGenerator* gen = &generator;
    gen->setName("s3test.pivotal.io")
        ->setPrefix("threebytes/")
        ->setIsTruncated(false)
        ->pushBuckentContent(BucketContent("threebytes/", 0))
        ->pushBuckentContent(BucketContent("threebytes/threebytes", 3));

    Response response(RESPONSE_OK, gen->toXML());

    EXPECT_CALL(mockRESTfulService, get(_, _)).WillOnce(Return(response));

    gpreader.open(this->params);

    const ListBucketResult& keyList = gpreader.getBucketReader().getKeyList();
    EXPECT_EQ((uint64_t)1, keyList.contents.size());

    gpreader.close();
    EXPECT_TRUE(keyList.contents.empty());
}

TEST_F(GPReaderTest, ReadSmallData) {
    string url = "s3://s3-us-west-2.amazonaws.com/s3test.pivotal.io/dataset1/normal";
    MockS3RESTfulService mockRESTfulService(this->params);
    MockGPReader gpreader(this->params, url, &mockRESTfulService);

    XMLGenerator generator;
    XMLGenerator* gen = &generator;
    gen->setName("s3test.pivotal.io")
        ->setPrefix("threebytes/")
        ->setIsTruncated(false)
        ->pushBuckentContent(BucketContent("threebytes/", 0))
        ->pushBuckentContent(BucketContent("threebytes/threebytes", 3));

    Response listBucketResponse(RESPONSE_OK, gen->toXML());

    vector<uint8_t> keyContent;

    // generate 3 bytes in random way
    srand(time(NULL));
    keyContent.push_back(rand() & 0xFF);
    keyContent.push_back(rand() & 0xFF);
    keyContent.push_back(rand() & 0xFF);

    Response keyReaderResponse(RESPONSE_OK, keyContent);

    EXPECT_CALL(mockRESTfulService, get(_, _))
        .WillOnce(Return(listBucketResponse))
        // first 4 bytes is retrieved once for format detection.
        .WillOnce(Return(keyReaderResponse))
        // whole file content
        .WillOnce(Return(keyReaderResponse));

    gpreader.open(this->params);

    const ListBucketResult& keyList = gpreader.getBucketReader().getKeyList();
    EXPECT_EQ((uint64_t)1, keyList.contents.size());
    EXPECT_EQ("threebytes/threebytes", keyList.contents[0].getName());
    EXPECT_EQ((uint64_t)3, keyList.contents[0].getSize());

    char buffer[64];
    EXPECT_EQ((uint64_t)3, gpreader.read(buffer, sizeof(buffer)));
    EXPECT_EQ(0, memcmp(buffer, keyContent.data(), 3));

    EXPECT_EQ((uint64_t)0, gpreader.read(buffer, sizeof(buffer)));
}

// We need to mock the Get() to fulfill unordered GET request.
class MockS3RESTfulServiceForMultiThreads : public MockS3RESTfulService {
   public:
    MockS3RESTfulServiceForMultiThreads(const S3Params& params, uint64_t dataSize)
        : MockS3RESTfulService(params) {
        srand(time(NULL));
        data.reserve(dataSize);
        for (uint64_t i = 0; i < dataSize; i++) {
            data.push_back(rand() & 0xFF);
        }
    }

    Response mockGet(const string& url, HTTPHeaders& headers) {
        string range = headers.Get(RANGE);
        size_t index = range.find("=");
        string rangeNumber = range.substr(index + 1);
        index = rangeNumber.find("-");

        // stoi is not available in C++98, use sscanf as workaround.
        int begin, end;
        if (index > 0) {
            sscanf(rangeNumber.substr(0, index).c_str(), "%d", &begin);
        } else {
            begin = 0;
        }

        if (rangeNumber.empty()) {
            end = data.size();
        } else {
            sscanf(rangeNumber.substr(index + 1).c_str(), "%d", &end);
        }

        vector<uint8_t> responseData(data.begin() + begin, data.begin() + end + 1);

        return Response(RESPONSE_OK, responseData);
    }

    const uint8_t* getData() {
        return data.data();
    }

   private:
    vector<uint8_t> data;
};

TEST_F(GPReaderTest, ReadHugeData) {
    InitConfig(this->params, "data/s3test.conf", "smallchunk");
    string url = "s3://s3-us-west-2.amazonaws.com/s3test.pivotal.io/dataset1/normal";

    // We don't know the chunksize before we load_config()
    //    so we create a big enough data (128M), to force
    //    multiple-threads to be constructed and loop-read to be triggered.
    uint64_t totalData = 1024 * 1024 * 128;

    MockS3RESTfulServiceForMultiThreads mockRESTfulService(this->params, totalData);
    MockGPReader gpreader(this->params, url, &mockRESTfulService);

    XMLGenerator generator;
    XMLGenerator* gen = &generator;
    gen->setName("s3test.pivotal.io")
        ->setPrefix("bigdata/")
        ->setIsTruncated(false)
        ->pushBuckentContent(BucketContent("bigdata/", 0))
        ->pushBuckentContent(BucketContent("bigdata/bigdata", totalData));

    Response listBucketResponse(RESPONSE_OK, gen->toXML());

    // call mockGet() instead of simply return a Response.
    EXPECT_CALL(mockRESTfulService, get(_, _))
        .WillOnce(Return(listBucketResponse))
        .WillRepeatedly(Invoke(&mockRESTfulService, &MockS3RESTfulServiceForMultiThreads::mockGet));

    gpreader.open(this->params);

    // compare the data size
    const ListBucketResult& keyList = gpreader.getBucketReader().getKeyList();
    EXPECT_EQ((uint64_t)1, keyList.contents.size());
    EXPECT_EQ("bigdata/bigdata", keyList.contents[0].getName());
    EXPECT_EQ(totalData, keyList.contents[0].getSize());

    // compare the data content
    static char buffer[1024 * 1024];
    uint64_t size = sizeof(buffer);
    for (uint64_t i = 0; i < totalData; i += size) {
        EXPECT_EQ(size, gpreader.read(buffer, size));
        ASSERT_EQ(0, memcmp(buffer, mockRESTfulService.getData() + i, size));
    }

    // Guarantee the last call
    EXPECT_EQ((uint64_t)0, gpreader.read(buffer, sizeof(buffer)));
}

TEST_F(GPReaderTest, ReadFromEmptyURL) {
    string url;
    MockS3RESTfulService mockRESTfulService(this->params);
    MockGPReader gpreader(this->params, url, &mockRESTfulService);

    // an exception should be throwed in parseURL()
    //    with message "'' is not valid"
    EXPECT_THROW(gpreader.open(this->params), S3ConfigError);
}

TEST_F(GPReaderTest, ReadFromInvalidURL) {
    string url = "s3://";

    MockS3RESTfulService mockRESTfulService(this->params);
    MockGPReader gpreader(this->params, url, &mockRESTfulService);

    // an exception should be throwed in parseURL()
    //    with message "'s3://' is not valid,"
    EXPECT_THROW(gpreader.open(this->params), S3ConfigError);
}

TEST_F(GPReaderTest, ReadAndGetFailedListBucketResponse) {
    string url = "s3://s3-us-west-2.amazonaws.com/s3test.pivotal.io/dataset1/normal";
    MockS3RESTfulService mockRESTfulService(this->params);
    MockGPReader gpreader(this->params, url, &mockRESTfulService);

    EXPECT_CALL(mockRESTfulService, get(_, _)).WillRepeatedly(Throw(S3FailedAfterRetry("", 3, "")));

    EXPECT_THROW(gpreader.open(this->params), S3FailedAfterRetry);
}

TEST_F(GPReaderTest, ReadAndGetFailedKeyReaderResponse) {
    string url = "s3://s3-us-west-2.amazonaws.com/s3test.pivotal.io/dataset1/normal";
    MockS3RESTfulService mockRESTfulService(this->params);
    MockGPReader gpreader(this->params, url, &mockRESTfulService);

    XMLGenerator generator;
    XMLGenerator* gen = &generator;
    gen->setName("s3test.pivotal.io")
        ->setPrefix("threebytes/")
        ->setIsTruncated(false)
        ->pushBuckentContent(BucketContent("threebytes/", 0))
        ->pushBuckentContent(BucketContent("threebytes/threebytes", 3));

    Response listBucketResponse(RESPONSE_OK, gen->toXML());

    EXPECT_CALL(mockRESTfulService, get(_, _))
        .WillOnce(Return(listBucketResponse))
        .WillRepeatedly(Throw(S3FailedAfterRetry("", 3, "")));

    gpreader.open(this->params);

    const ListBucketResult& keyList = gpreader.getBucketReader().getKeyList();
    EXPECT_EQ((uint64_t)1, keyList.contents.size());
    EXPECT_EQ("threebytes/threebytes", keyList.contents[0].getName());
    EXPECT_EQ((uint64_t)3, keyList.contents[0].getSize());

    char buffer[64];
    EXPECT_THROW(gpreader.read(buffer, sizeof(buffer)), S3FailedAfterRetry);
}

// thread functions test with local variables
TEST(Common, ThreadFunctions) {
    // just to test if these two are functional
    thread_setup();
    EXPECT_NE((void*)NULL, mutex_buf);

    thread_cleanup();
    EXPECT_EQ((void*)NULL, mutex_buf);

    EXPECT_NE((uint64_t)0, id_function());
}
