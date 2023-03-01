#!/usr/bin/env python3

"""The lambda to decrease/increase ASG desired capacity based on current queue"""

import json
import logging
import time
from dataclasses import dataclass
from pprint import pformat
from typing import Any, List, Literal, Optional

import boto3  # type: ignore
import requests  # type: ignore

RUNNER_TYPE_LABELS = [
    "builder",
    "func-tester",
    "func-tester-aarch64",
    "fuzzer-unit-tester",
    "stress-tester",
    "style-checker",
    "style-checker-aarch64",
]
QUEUE_QUERY = f"""SELECT
    last_status AS status,
    toUInt32(count()) AS length,
    labels
FROM
(
    SELECT
        arraySort(groupArray(status))[-1] AS last_status,
        labels,
        id,
        html_url
    FROM default.workflow_jobs
    WHERE has(labels, 'self-hosted')
        AND hasAny({RUNNER_TYPE_LABELS}, labels)
        AND started_at > now() - INTERVAL 2 DAY
    GROUP BY ALL
    HAVING last_status IN ('in_progress', 'queued')
)
GROUP BY ALL
ORDER BY labels, last_status"""


@dataclass
class Queue:
    status: Literal["in_progress", "queued"]
    lentgh: int
    label: str


### VENDORING
def get_parameter_from_ssm(name, decrypt=True, client=None):
    if not client:
        client = boto3.client("ssm", region_name="us-east-1")
    return client.get_parameter(Name=name, WithDecryption=decrypt)["Parameter"]["Value"]


class CHException(Exception):
    pass


class ClickHouseHelper:
    def __init__(
        self,
        url: Optional[str] = None,
        user: Optional[str] = None,
        password: Optional[str] = None,
    ):
        self.url = url
        self.auth = {}
        if user:
            self.auth["X-ClickHouse-User"] = user
        if password:
            self.auth["X-ClickHouse-Key"] = password

    def _select_and_get_json_each_row(self, db, query):
        params = {
            "database": db,
            "query": query,
            "default_format": "JSONEachRow",
        }
        for i in range(5):
            response = None
            try:
                response = requests.get(self.url, params=params, headers=self.auth)
                response.raise_for_status()
                return response.text
            except Exception as ex:
                logging.warning("Cannot fetch data with exception %s", str(ex))
                if response:
                    logging.warning("Reponse text %s", response.text)
                time.sleep(0.1 * i)

        raise CHException("Cannot fetch data from clickhouse")

    def select_json_each_row(self, db, query):
        text = self._select_and_get_json_each_row(db, query)
        result = []
        for line in text.split("\n"):
            if line:
                result.append(json.loads(line))
        return result


CH_CLIENT = ClickHouseHelper(get_parameter_from_ssm("clickhouse-test-stat-url"), "play")


def set_capacity(
    runner_type: str, queues: List[Queue], client: Any, dry_run: bool = True
) -> None:
    assert len(queues) in (1, 2)
    assert all(q.label == runner_type for q in queues)
    as_groups = client.describe_auto_scaling_groups(
        Filters=[
            {"Name": "tag-key", "Values": ["github:runner-type"]},
            {"Name": "tag-value", "Values": [runner_type]},
        ]
    )["AutoScalingGroups"]
    assert len(as_groups) == 1
    asg = as_groups[0]
    running = 0
    queued = 0
    for q in queues:
        if q.status == "in_progress":
            running = q.lentgh
            continue
        if q.status == "queued":
            queued = q.lentgh
            continue
        raise ValueError("Queue status is not in ['in_progress', 'queued']")

    capacity_reserve = max(0, asg["DesiredCapacity"] - running)
    stop = False
    if queued:
        # This part is about scaling up
        # First, let's check if there's enough runners to cover the queue
        stop = stop or (asg["DesiredCapacity"] - running - queued) > 0

        stop = stop or asg["MaxSize"] <= asg["DesiredCapacity"]
        # Let's calculate a new desired capacity. Here the scale is used to not
        # scale up and down too quickly
        desired_capacity = asg["DesiredCapacity"] + ((queued - capacity_reserve) // 5)
        desired_capacity = max(desired_capacity, asg["MinSize"])
        desired_capacity = min(desired_capacity, asg["MaxSize"])
        # Finally, should the capacity be even changed
        stop = stop or asg["DesiredCapacity"] == desired_capacity
        if stop:
            return
        logging.info(
            "The ASG %s capacity will be increased to %s, current capacity=%s, "
            "maximum capacity=%s, running jobs=%s, queue size=%s",
            asg["AutoScalingGroupName"],
            desired_capacity,
            asg["DesiredCapacity"],
            asg["MaxSize"],
            running,
            queued,
        )
        if not dry_run:
            client.set_desired_capacity(
                AutoScalingGroupName=asg["AutoScalingGroupName"],
                DesiredCapacity=desired_capacity,
            )
        return

    # Now we will calculate if we need to scale down
    stop = stop or asg["DesiredCapacity"] <= asg["MinSize"]
    stop = stop or asg["DesiredCapacity"] <= running
    # Scale down quicker than scale up
    desired_capacity = asg["DesiredCapacity"] - (capacity_reserve // 3)
    desired_capacity = max(desired_capacity, asg["MinSize"])
    desired_capacity = min(desired_capacity, asg["MaxSize"])
    stop = stop or asg["DesiredCapacity"] == desired_capacity
    if stop:
        return

    logging.info(
        "The ASG %s capacity will be decreased to %s, current capacity=%s, "
        "minimum capacity=%s, running jobs=%s, queue size=%s",
        asg["AutoScalingGroupName"],
        desired_capacity,
        asg["DesiredCapacity"],
        asg["MinSize"],
        running,
        queued,
    )
    if not dry_run:
        client.set_desired_capacity(
            AutoScalingGroupName=asg["AutoScalingGroupName"],
            DesiredCapacity=desired_capacity,
        )


def main(dry_run: bool = True) -> None:
    logging.getLogger().setLevel(logging.INFO)
    asg_client = boto3.client("autoscaling")
    try:
        global CH_CLIENT
        queues = CH_CLIENT.select_json_each_row("default", QUEUE_QUERY)
    except CHException as ex:
        logging.exception(
            "Got an exception on insert, tryuing to update the client "
            "credentials and repeat",
            exc_info=ex,
        )
        CH_CLIENT = ClickHouseHelper(
            get_parameter_from_ssm("clickhouse-test-stat-url"), "play"
        )
        queues = CH_CLIENT.select_json_each_row("default", QUEUE_QUERY)

    logging.info("Received queue data:\n%s", pformat(queues, width=120))
    for runner_type in RUNNER_TYPE_LABELS:
        runner_queues = [
            Queue(queue["status"], queue["length"], runner_type)
            for queue in queues
            if runner_type in queue["labels"]
        ]
        set_capacity(runner_type, runner_queues, asg_client, dry_run)


def handler(event: dict, context: Any) -> None:
    _ = event
    _ = context
    return main(False)
