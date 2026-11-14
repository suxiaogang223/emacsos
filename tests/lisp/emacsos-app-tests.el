;;; emacsos-app-tests.el --- Application policy tests -*- lexical-binding: t; -*-
;; SPDX-License-Identifier: GPL-3.0-or-later

(require 'ert)
(require 'emacsos-app)
(require 'emacsos-counter)

(defmacro emacsos-test--isolated (&rest body)
  "Run BODY with independent registries, then stop all surviving instances."
  (declare (indent 0) (debug t))
  `(let ((emacsos-app--runs (make-hash-table :test #'eql))
         (emacsos-app--next-id 0) (emacsos-app--next-resource 0)
         (emacsos-app-current nil))
     (unwind-protect (progn ,@body)
       (dolist (run (emacsos-app-list))
         (emacsos-app-request-stop (plist-get run :id))))))

(defun emacsos-test--idle ()
  (emacsos-app-start "Idle" #'ignore))

(ert-deftest emacsos-app-entry-return-is-not-exit ()
  (emacsos-test--isolated
    (let ((id (emacsos-test--idle)))
      (should (eq 'running (plist-get (emacsos-app-inspect id) :state)))
      (should (eq 'pending (plist-get (emacsos-app-wait id) :status)))
      (emacsos-app-finish id "Done")
      (let ((run (emacsos-app-inspect id)))
        (should (eq 'exited (plist-get run :state)))
        (should (eq 'finished (plist-get run :reason)))
        (should (equal "Done" (plist-get run :result))))
      (should (eq 'complete (plist-get (emacsos-app-wait id) :status))))))

(ert-deftest emacsos-app-context-is-captured-and-restored ()
  (emacsos-test--isolated
    (let* ((outer (emacsos-test--idle)) (inner (emacsos-test--idle)) values
           (inside (emacsos-app-wrap inner (lambda () (push emacsos-app-current values))))
           (outside (emacsos-app-wrap
                     outer (lambda ()
                             (push emacsos-app-current values)
                             (funcall inside)
                             (push emacsos-app-current values)))))
      (let ((emacsos-app-current 'caller))
        (funcall outside)
        (should (eq 'caller emacsos-app-current)))
      (should (equal (list outer inner outer) (nreverse values))))))

(ert-deftest emacsos-app-stop-defers-cleanup-to-all-active-callbacks ()
  (emacsos-test--isolated
    (let ((id (emacsos-test--idle)) events nested)
      (emacsos-app-adopt id (list 'resource)
                         (lambda (_) (push 'released events)))
      (setq nested
            (emacsos-app-wrap
             id (lambda ()
                  (emacsos-app-request-stop id)
                  (should (eq 'stop-requested (plist-get (emacsos-app-inspect id) :state)))
                  (should (= 2 (plist-get (emacsos-app-inspect id) :active)))
                  (push 'inner events))))
      (funcall (emacsos-app-wrap
                id (lambda ()
                     (funcall nested)
                     (should-not (memq 'released events))
                     (push 'outer events))))
      (should (equal '(inner outer released) (nreverse events)))
      (should (eq 'exited (plist-get (emacsos-app-inspect id) :state)))
      (emacsos-app-request-stop id)
      (should (= 1 (cl-count 'released events))))))

(ert-deftest emacsos-app-finish-invalidates-reentrant-and-stale-delivery ()
  (emacsos-test--isolated
    (let ((id (emacsos-test--idle)) (calls 0) callback)
      (setq callback (emacsos-app-wrap id (lambda () (cl-incf calls))))
      (funcall (emacsos-app-wrap
                id (lambda ()
                     (emacsos-app-finish id)
                     (funcall callback)
                     (should (eq 'completing (plist-get (emacsos-app-inspect id) :state))))))
      (funcall callback)
      (should (= 0 calls)))))

(ert-deftest emacsos-app-partial-start-error-retains-cause-and-attempts-cleanup ()
  (emacsos-test--isolated
    (let (released)
      (should-error
       (emacsos-app-start
        "Broken" (lambda ()
                   (emacsos-app-adopt emacsos-app-current (list 'first)
                                      (lambda (_) (push 'first released)))
                   (emacsos-app-adopt emacsos-app-current (list 'second)
                                      (lambda (_) (push 'second released)
                                        (error "Cleanup failure")))
                   (error "Original startup failure"))))
      (let ((run (emacsos-app-inspect 1)))
        (should (eq 'failed (plist-get run :state)))
        (should (string-match-p "Original startup failure" (plist-get run :error)))
        (should (string-match-p "Cleanup failure" (car (plist-get run :cleanup-errors))))
        (should-not (plist-get run :resources)))
      (should (equal '(first second) released))
      (should-not emacsos-app-current))))

(ert-deftest emacsos-app-quit-and-throw-restore-context-and-counts ()
  (emacsos-test--isolated
    (let ((id (emacsos-test--idle)) released)
      (emacsos-app-adopt id (list 'resource) (lambda (_) (setq released t)))
      (let ((emacsos-app-current 'caller))
        (should (eq 'escaped
                    (catch 'escape
                      (funcall (emacsos-app-wrap id (lambda () (throw 'escape 'escaped)))))))
        (should (eq 'caller emacsos-app-current)))
      (should released)
      (should (= 0 (plist-get (emacsos-app-inspect id) :active)))
      (should (string-match-p "nonlocal-exit" (plist-get (emacsos-app-inspect id) :error))))
    (let ((id (emacsos-test--idle)) got-quit)
      (condition-case nil
          (funcall (emacsos-app-wrap id (lambda () (signal 'quit nil))))
        (quit (setq got-quit t)))
      (should got-quit)
      (should (eq 'failed (plist-get (emacsos-app-inspect id) :state)))
      (should-not emacsos-app-current))))

(ert-deftest emacsos-app-cleanup-is-reentrant-and-consumes-before-release ()
  (emacsos-test--isolated
    (let ((id (emacsos-test--idle)) (calls 0))
      (emacsos-app-adopt
       id (list 'resource)
       (lambda (_)
         (cl-incf calls)
         (should (eq 'cleaning (plist-get (emacsos-app-inspect id) :state)))
         (should (eq 'pending (plist-get (emacsos-app-wait id) :status)))
         (emacsos-app-request-stop id)
         (should-error (emacsos-app-restart id))
         (should-error (emacsos-app-adopt id (list 'late) #'ignore))))
      (emacsos-app-finish id)
      (should (= calls 1))
      (should (eq 'exited (plist-get (emacsos-app-inspect id) :state))))))

(ert-deftest emacsos-app-restart-needs-observed-terminal-and-fresh-identity ()
  (emacsos-test--isolated
    (let* ((id (emacsos-test--idle)) (old (emacsos-app-wrap id (lambda () (error "Stale")))))
      (should-error (emacsos-app-restart id))
      ;; Internal completion here intentionally avoids a public outcome observation.
      (emacsos-app--request (emacsos-app--get id) 'finished)
      (emacsos-app-list)
      (should-error (emacsos-app-restart id))
      (emacsos-app-wait id)
      (let ((fresh (emacsos-app-restart id)))
        (should (/= fresh id))
        (funcall old)
        (should (eq 'running (plist-get (emacsos-app-inspect fresh) :state)))))))

(ert-deftest emacsos-app-borrowing-sharing-and-transfer-have-explicit-ownership ()
  (emacsos-test--isolated
    (let* ((a (emacsos-test--idle)) (b (emacsos-test--idle)) (c (emacsos-test--idle))
           (value (list 'shared)) (calls 0)
           (resource (emacsos-app-adopt a value (lambda (_) (cl-incf calls))))
           (borrowed (emacsos-app-adopt b value (lambda (_) (error "Borrow released"))
                                       'custom 'borrowed)))
      (should-error (emacsos-app-adopt b value #'ignore))
      (should-error (emacsos-app-share b borrowed c))
      (emacsos-app-share a resource b)
      (emacsos-app-transfer a resource c)
      (should-not (plist-get (emacsos-app-inspect a) :resources))
      (emacsos-app-request-stop a)
      (emacsos-app-request-stop b)
      (should (= 0 calls))
      (emacsos-app-request-stop c)
      (should (= 1 calls)))))

(ert-deftest emacsos-app-failed-transfer-keeps-source-responsible ()
  (emacsos-test--isolated
    (let* ((a (emacsos-test--idle)) (b (emacsos-test--idle)) (calls 0)
           (resource (emacsos-app-adopt a (list 'resource) (lambda (_) (cl-incf calls)))))
      (emacsos-app-request-stop b)
      (should-error (emacsos-app-transfer a resource b))
      (should (= 1 (length (plist-get (emacsos-app-inspect a) :resources))))
      (emacsos-app-request-stop a)
      (should (= 1 calls)))))

(ert-deftest emacsos-app-existing-and-modified-buffers-are-preserved ()
  (emacsos-test--isolated
    (let* ((id (emacsos-test--idle))
           (borrowed (generate-new-buffer " *borrowed*"))
           (owned (emacsos-app-buffer id " *owned*"))
           (modified (emacsos-app-buffer id " *modified*")))
      (unwind-protect
          (progn
            (emacsos-app-adopt-buffer id borrowed)
            (with-current-buffer modified (insert "User work"))
            (emacsos-app-request-stop id)
            (should (buffer-live-p borrowed))
            (should-not (buffer-live-p owned))
            (should (buffer-live-p modified))
            (should (equal "User work" (with-current-buffer modified (buffer-string))))
            (should (cl-some (lambda (line) (string-match-p "preserved-modified" line))
                             (plist-get (emacsos-app-inspect id) :logs))))
        (dolist (buffer (list borrowed modified))
          (when (buffer-live-p buffer)
            (with-current-buffer buffer (set-buffer-modified-p nil))
            (kill-buffer buffer)))))))

(ert-deftest emacsos-app-records-logs-and-registrations-are-bounded ()
  (emacsos-test--isolated
    (let ((emacsos-app-max-records 2) (emacsos-app-max-resources 1)
          (emacsos-app-max-logs 2))
      (let ((a (emacsos-test--idle)) (b (emacsos-test--idle)))
        (should-error (emacsos-test--idle))
        (emacsos-app-wrap a #'ignore)
        (should-error (emacsos-app-wrap a #'ignore))
        (dotimes (i 4) (emacsos-app-log a (format "Log %d" i)))
        (let ((snapshot (emacsos-app-inspect a)))
          (should (= 2 (length (plist-get snapshot :logs))))
          (should (= 3 (plist-get snapshot :lost-logs)))
          (setcar (plist-get snapshot :logs) "Corrupted snapshot")
          (should-not (member "Corrupted snapshot" (plist-get (emacsos-app-inspect a) :logs))))
        (let ((stale (emacsos-app--resource-value
                      (car (emacsos-app--run-resources (emacsos-app--get a))))))
          (should stale))
        (emacsos-app-request-stop a)
        (let ((fresh (emacsos-test--idle)))
          (should (> fresh b))
          (should-error (emacsos-app-inspect a) :type 'emacsos-app-expired)
          (should (eq 'running (plist-get (emacsos-app-inspect b) :state))))))))

(ert-deftest emacsos-app-explicit-release-is-once-even-on-error ()
  (emacsos-test--isolated
    (let* ((id (emacsos-test--idle)) (calls 0)
           (resource (emacsos-app-adopt id (list 'value)
                                       (lambda (_) (cl-incf calls) (error "Close failed")))))
      (emacsos-app-release id resource)
      (should-error (emacsos-app-release id resource))
      (emacsos-app-request-stop id)
      (should (= 1 calls))
      (should (eq 'failed (plist-get (emacsos-app-inspect id) :state)))
      (should (eq 'stopped (plist-get (emacsos-app-inspect id) :reason))))))

(ert-deftest emacsos-app-timer-acquisition-failure-rolls-back-registration ()
  (emacsos-test--isolated
    (let ((id (emacsos-test--idle)))
      (should-error (emacsos-app-run-at-time id '(invalid time) 1 #'ignore))
      (should-not (plist-get (emacsos-app-inspect id) :resources))
      (should (eq 'running (plist-get (emacsos-app-inspect id) :state))))))

(ert-deftest emacsos-app-repeated-release-errors-have-bounded-diagnostics ()
  (emacsos-test--isolated
    (let ((id (emacsos-test--idle)) (emacsos-app-max-cleanup-errors 2))
      (dotimes (_ 5)
        (emacsos-app-release
         id (emacsos-app-adopt id (list 'value) (lambda (_) (error "Release failed")))))
      (let ((snapshot (emacsos-app-inspect id)))
        (should (= 2 (length (plist-get snapshot :cleanup-errors))))
        (should (= 3 (plist-get snapshot :lost-cleanup-errors)))))))

(ert-deftest emacsos-app-buffer-kill-veto-returns-ownership ()
  (emacsos-test--isolated
    (let* ((id (emacsos-test--idle)) (buffer (emacsos-app-buffer id " *veto*")))
      (unwind-protect
          (progn
            (with-current-buffer buffer
              (setq-local kill-buffer-query-functions (list (lambda () nil))))
            (emacsos-app-request-stop id)
            (should (buffer-live-p buffer))
            (should-not (plist-get (emacsos-app-inspect id) :resources)))
        (when (buffer-live-p buffer)
          (with-current-buffer buffer (setq kill-buffer-query-functions nil))
          (kill-buffer buffer))))))

(ert-deftest emacsos-app-cleanup-throw-attempts-rest-and-records-terminal ()
  (emacsos-test--isolated
    (let ((id (emacsos-test--idle)) remaining)
      (emacsos-app-adopt id (list 'first) (lambda (_) (setq remaining t)))
      (emacsos-app-adopt id (list 'second) (lambda (_) (throw 'bad-release 'escaped)))
      (should (eq 'escaped (catch 'bad-release (emacsos-app-request-stop id))))
      (should remaining)
      (should (eq 'failed (plist-get (emacsos-app-inspect id) :state)))
      (should (plist-get (emacsos-app-inspect id) :cleanup-errors)))))

(ert-deftest emacsos-app-buffer-hook-edits-are-preserved-during-cleanup ()
  (emacsos-test--isolated
    (dolist (hook '(kill-buffer-query-functions kill-buffer-hook))
      (let* ((id (emacsos-test--idle)) (buffer (emacsos-app-buffer id " *hook-edit*")))
        (unwind-protect
            (progn
              (with-current-buffer buffer
                (set (make-local-variable hook)
                     (list (lambda () (insert "Late user work") t))))
              (emacsos-app-request-stop id)
              (should (buffer-live-p buffer))
              (should (equal "Late user work" (with-current-buffer buffer (buffer-string)))))
          (when (buffer-live-p buffer)
            (with-current-buffer buffer
              (setq kill-buffer-query-functions nil kill-buffer-hook nil)
              (set-buffer-modified-p nil))
            (kill-buffer buffer)))))))

(ert-deftest emacsos-counter-two-instances-stop-and-restart-independently ()
  (emacsos-test--isolated
    (let* ((a (emacsos-counter-start)) (b (emacsos-counter-start))
           (a-buffer (get-buffer (format "*Counter %d*" a)))
           (b-buffer (get-buffer (format "*Counter %d*" b)))
           (a-timer (cl-find 'timer (emacsos-app--run-resources (emacsos-app--get a))
                            :key #'emacsos-app--resource-kind))
           (old-timer (emacsos-app--resource-value a-timer)))
      ;; Drive the actual timer callback deterministically, without sleeping.
      (apply (timer--function old-timer) (timer--args old-timer))
      (with-current-buffer a-buffer (goto-char (point-max)) (insert "User edit"))
      (emacsos-app-request-stop a)
      (should (buffer-live-p a-buffer))
      (should-not (memq old-timer timer-list))
      (let* ((b-timer (emacsos-app--resource-value
                       (cl-find 'timer (emacsos-app--run-resources (emacsos-app--get b))
                                :key #'emacsos-app--resource-kind)))
             (before (with-current-buffer b-buffer (buffer-string))))
        (apply (timer--function b-timer) (timer--args b-timer))
        (should-not (equal before (with-current-buffer b-buffer (buffer-string)))))
      (let ((fresh (emacsos-app-restart a)))
        (should (/= a fresh))
        (should (buffer-live-p (get-buffer (format "*Counter %d*" fresh))))
        (let ((before (with-current-buffer a-buffer (buffer-string))))
          (apply (timer--function old-timer) (timer--args old-timer))
          (should (equal before (with-current-buffer a-buffer (buffer-string)))))
        (emacsos-app-request-stop fresh))
      (with-current-buffer a-buffer (set-buffer-modified-p nil))
      (kill-buffer a-buffer))))

(ert-deftest emacsos-processes-ui-uses-the-management-contract ()
  (emacsos-test--isolated
    (let ((id (emacsos-test--idle)))
      (emacsos-processes)
      (should (derived-mode-p 'emacsos-processes-mode))
      (goto-char (point-min))
      (should (eql id (tabulated-list-get-id)))
      (emacsos-processes-stop)
      (should (eq 'exited (plist-get (emacsos-app-inspect id) :state)))
      (emacsos-processes-restart)
      (should (= 2 (length (emacsos-app-list))))
      (kill-buffer (current-buffer)))))

;;; emacsos-app-tests.el ends here
