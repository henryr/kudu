// Copyright (c) 2014, Cloudera, inc.
package kudu.rpc;

/**
 * Interface that defines the methods used to configure a session. It also exposes ways to
 * query its state.
 */
public interface SessionConfiguration {

  public enum FlushMode {
    // Every write will be sent to the server in-band with the Apply()
    // call. No batching will occur. This is the default flush mode. In this
    // mode, the Flush() call never has any effect, since each Apply() call
    // has already flushed the buffer.
    AUTO_FLUSH_SYNC,

    // Apply() calls will return immediately, but the writes will be sent in
    // the background, potentially batched together with other writes from
    // the same session. If there is not sufficient buffer space, then Apply()
    // may block for buffer space to be available.
    //
    // Because writes are applied in the background, any errors will be stored
    // in a session-local buffer. Call CountPendingErrors() or GetPendingErrors()
    // to retrieve them.
    //
    // The Flush() call can be used to block until the buffer is empty.
    AUTO_FLUSH_BACKGROUND,

    // Apply() calls will return immediately, and the writes will not be
    // sent until the user calls Flush(). If the buffer runs past the
    // configured space limit, then Apply() will return an error.
    MANUAL_FLUSH
  }

  /**
   * Get the current flush mode.
   * @return flush mode, AUTO_FLUSH_SYNC by default
   */
  public FlushMode getFlushMode();

  /**
   * Set the new flush mode for this session.
   * @param flushMode new flush mode, can be the same as the previous one.
   * @throws IllegalArgumentException if the buffer isn't empty.
   */
  public void setFlushMode(FlushMode flushMode);

  /**
   * Set the number of operations that can be buffered.
   * @param size number of ops.
   * @throws IllegalArgumentException if the buffer isn't empty.
   */
  public void setMutationBufferSpace(int size);

  /**
   * Set the low watermark for this session. The default is set to half the mutation buffer space.
   * For example, a buffer space of 1000 with a low watermark set to 50% (0.5) will start randomly
   * sending PleaseRetryExceptions once there's an outstanding flush and the buffer is over 500.
   * As the buffer gets fuller, it becomes likelier to hit the exception.
   * @param mutationBufferLowWatermark New low watermark as a percentage,
   *                             has to be between 0  and 1 (inclusive). A value of 1 disables
   *                             the low watermark since it's the same as the high one.
   * @throws IllegalArgumentException if the buffer isn't empty or if the watermark isn't between
   * 0 and 1.
   */
  public void setMutationBufferLowWatermark(float mutationBufferLowWatermark);

  /**
   * Set the flush interval, which will be used for the next scheduling decision.
   * @param interval interval in milliseconds.
   */
  public void setFlushInterval(int interval);

  /**
   * Get the current timeout.
   * @return operation timeout in milliseconds, 0 if none was configured.
   */
  public long getTimeoutMillis();

  /**
   * Sets the timeout for the next applied operations.
   * The default timeout is 0, which disables the timeout functionality.
   * @param timeout Timeout in milliseconds.
   */
  public void setTimeoutMillis(long timeout);

  /**
   * Returns true if this session has already been closed.
   */
  public boolean isClosed();

  /**
   * Check if there are operations that haven't been completely applied.
   * @return true if operations are pending, else false.
   */
  public boolean hasPendingOperations();

  /**
   * Set the new external consistency mode for this session.
   * @param consistencyMode new external consistency mode, can the same as the previous one.
   * @throws IllegalArgumentException if the buffer isn't empty.
   */
  public void setExternalConsistencyMode(ExternalConsistencyMode consistencyMode);
}
